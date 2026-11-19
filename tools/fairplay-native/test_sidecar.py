"""Exercise actual freestanding C loader validation against synthetic payloads."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from sidecar import checksum, HEADER, ENTRY

CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint64)


def fixture():
    identity = bytes(range(16))
    commands = []
    for index, name in enumerate(('__PRELINK_TEXT', '__PLK_TEXT_EXEC', '__PLK_DATA_CONST', '__PRELINK_DATA', '__TEXT')):
        command = bytearray(72)
        struct.pack_into('<II16sQQ', command, 0, 0x19, 72, name.encode(), 0x10000 * (index + 1), 0x1000)
        commands.append(command)
    commands.append(struct.pack('<II16s', 0x1b, 24, identity))
    kernel = bytearray(32) + b''.join(commands)
    struct.pack_into('<IIIIII', kernel, 0, 0xfeedfacf, 0x0100000c, 0, 2, len(commands), len(kernel) - 32)
    block = b'synthetic native bytes'
    blob = bytearray(32 + 2 * 32)
    for index in range(2):
        offset = len(blob)
        blob.extend(block)
        ENTRY.pack_into(blob, 32 + index * 32, 0x10000 * (index + 1), len(block), offset, checksum(block), 0)
    HEADER.pack_into(blob, 0, b'RFP1', 1, 2, len(blob), identity)
    return kernel, blob


class SidecarTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='redwood-sidecar-test-')
        directory = Path(cls.temp.name)
        source = directory / 'wrapper.c'
        source.write_text('#include "native_sidecar.h"\nunsigned validate(const uint8_t *k, uint64_t kn, const uint8_t *b, uint64_t bn, rwfp_copy_fn c, void *x) { return rwfp_apply(k, kn, b, bn, c, x); }\n')
        library = directory / 'validator.dylib'
        subprocess.run(['/usr/bin/clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                        '-I', str(Path(__file__).parent), str(source), '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.validate = cls.library.validate
        cls.validate.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint64, CALLBACK, ctypes.c_void_p]
        cls.validate.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_payload(self, kernel, blob):
        copies = []
        callback = CALLBACK(lambda _, address, source, size: copies.append((address, ctypes.string_at(source, size))))
        kb, bb = ctypes.create_string_buffer(bytes(kernel)), ctypes.create_string_buffer(bytes(blob))
        count = self.validate(kb, len(kernel), bb, len(blob), callback, None)
        return count, copies

    def test_valid_payload_copies_exact_chunks(self):
        kernel, blob = fixture()
        count, copies = self.run_payload(kernel, blob)
        self.assertEqual(count, 2)
        self.assertEqual([a for a, b in copies], [0x10000, 0x20000])
        self.assertTrue(all(b == b'synthetic native bytes' for a, b in copies))

    def test_bad_second_checksum_produces_no_partial_copy(self):
        kernel, blob = fixture()
        blob[-1] ^= 1
        self.assertEqual(self.run_payload(kernel, blob), (0, []))

    def test_rejects_truncation_uuid_and_oversized_table(self):
        kernel, blob = fixture()
        variants = [blob[:20], blob[:-1]]
        wrong_uuid, wrong_count = bytearray(blob), bytearray(blob)
        wrong_uuid[16] ^= 1
        struct.pack_into('<I', wrong_count, 8, 17)
        variants.extend([wrong_uuid, wrong_count])
        for variant in variants:
            self.assertEqual(self.run_payload(kernel, variant), (0, []))

    def test_rejects_kernel_targets_overflow_and_overlap(self):
        kernel, blob = fixture()
        for address in (0x50000, 0xfffffffffffffff0, 0x10000):
            variant = bytearray(blob)
            struct.pack_into('<Q', variant, 64, address)
            self.assertEqual(self.run_payload(kernel, variant), (0, []))

    def test_rejects_duplicate_groups_and_kernel_overlap(self):
        kernel, blob = fixture()
        duplicate = bytearray(kernel)
        duplicate[32 + 72 + 8:32 + 72 + 24] = kernel[32 + 8:32 + 24]
        self.assertEqual(self.run_payload(duplicate, blob), (0, []))
        overlapping = bytearray(kernel)
        struct.pack_into('<Q', overlapping, 32 + 4 * 72 + 24, 0x10000)
        self.assertEqual(self.run_payload(overlapping, blob), (0, []))


if __name__ == '__main__':
    unittest.main()

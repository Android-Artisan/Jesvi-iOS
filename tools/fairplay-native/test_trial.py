"""Packaging checks for preserved boot parameters and fixed payload bounds."""
import struct
import unittest

from prepare_trial import PAYLOAD_AT, PAYLOAD_MAX, read_payload, write_payload


class TrialTests(unittest.TestCase):
    def test_payload_roundtrip_preserves_binary_loader_and_boot_parameters(self):
        entries = [('loader.efi', b'MZ\0\xff\0'), ('cmdline', b'softpasscode=1 autounlock=0\0'),
                   ('rsize', b'd00000\0'), ('fpart', b'recovery\0'), ('foff', b'1800000\0')]
        payload = write_payload(entries)
        image = b'\0' * PAYLOAD_AT + payload
        self.assertEqual(read_payload(image), entries)

    def test_oversized_or_duplicate_payload_is_rejected(self):
        with self.assertRaises(ValueError):
            write_payload([('loader.efi', b'x' * PAYLOAD_MAX)])
        with self.assertRaises(ValueError):
            write_payload([('cmdline', b'a\0'), ('cmdline', b'b\0')])

    def test_truncated_entry_or_missing_terminator_is_rejected(self):
        payload = bytearray(write_payload([('cmdline', b'a\0')]))
        struct.pack_into('<I', payload, 8 + 16, 0xffffffff)
        with self.assertRaises(ValueError):
            read_payload(b'\0' * PAYLOAD_AT + payload)
        payload = bytearray(write_payload([('cmdline', b'a\0')]))
        payload[-24:] = b'x' * 24
        with self.assertRaises(ValueError):
            read_payload(b'\0' * PAYLOAD_AT + payload)


if __name__ == '__main__':
    unittest.main()

"""Run the loader's real C validator on a generated payload, without execution."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from sidecar import validate


def verify(directory):
    kernel = (directory / 'kernel.macho').read_bytes()
    payload = (directory / 'native-fairplay.sidecar').read_bytes()
    expected = validate(kernel, payload)
    here = Path(__file__).resolve().parent
    runtime_header = here.parent.parent / 'xnuloader/native_sidecar.h'
    if runtime_header.read_bytes() != (here / 'native_sidecar.h').read_bytes():
        raise ValueError('Runtime and tested validator headers differ')
    callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint64)
    with tempfile.TemporaryDirectory(prefix='redwood-native-validate-') as temporary:
        temp = Path(temporary)
        source, library = temp / 'check.c', temp / 'check.dylib'
        source.write_text('#include "native_sidecar.h"\nunsigned check(const uint8_t *k, uint64_t kn, const uint8_t *b, uint64_t bn, rwfp_copy_fn c) { return rwfp_apply(k, kn, b, bn, c, 0); }\n')
        subprocess.run(['/usr/bin/clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                        '-I', str(here), str(source), '-o', str(library)], check=True)
        loaded = ctypes.CDLL(str(library))
        call = loaded.check
        call.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint64, callback_type]
        call.restype = ctypes.c_uint
        copied = []
        callback = callback_type(lambda _, address, content, size: copied.append((address, ctypes.string_at(content, size))))
        k, b = ctypes.create_string_buffer(kernel), ctypes.create_string_buffer(payload)
        count = call(k, len(kernel), b, len(payload), callback)
        if count != len(expected) or copied != expected:
            raise ValueError('C validator output differs from verified Python reconstruction')
    result = dict(c_validator_pass=True, pieces=count, exact_python_reference_match=True,
                  kernel_sha256=hashlib.sha256(kernel).hexdigest(),
                  sidecar_sha256=hashlib.sha256(payload).hexdigest(),
                  validator_sha256=hashlib.sha256(runtime_header.read_bytes()).hexdigest())
    (directory / 'c-validator-check.json').write_text(json.dumps(result, indent=2) + '\n')
    print('C validator verified', count, 'native pieces; no Apple code executed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    verify(args.directory)

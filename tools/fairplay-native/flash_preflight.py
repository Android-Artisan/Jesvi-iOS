"""Read-only verification before the explicit layout-trial RECOVERY write path."""
import argparse
import hashlib
import json
from pathlib import Path

from prepare_trial import read_payload, PARTITION_SIZE, SIDECAR_AT, KERNEL_AT
from sidecar import validate


def verify(image_path, manifest_path):
    image = image_path.read_bytes()
    manifest = json.loads(manifest_path.read_text())
    digest = lambda data: hashlib.sha256(data).hexdigest()
    if manifest.get('status') != 'PREPARED_LAYOUT_ONLY_NOT_FLASHED' or manifest.get('fairplay_native') != 0:
        raise ValueError('Only the prepared layout-only image is accepted')
    if not all(manifest.get(k) is True for k in ('template_boot_arguments_preserved', 'ramdisk_preserved', 'avb_verified')):
        raise ValueError('Incomplete image validation record')
    if len(image) != PARTITION_SIZE or digest(image) != manifest['image_sha256']:
        raise ValueError('Recovery image changed after validation')
    entries = read_payload(image)
    payload = dict(entries)
    if len(payload) != len(entries):
        raise ValueError('Duplicate payload entries')
    if payload.get('fpart') != b'recovery\0' or payload.get('foff') != b'1800000\0':
        raise ValueError('Unexpected native sidecar storage location')
    count = int(payload['fsize'].rstrip(b'\0'), 16)
    if count != manifest['sidecar_bytes'] or not 32 <= count <= 0x400000:
        raise ValueError('Invalid sidecar extent')
    size = manifest['kernel_bytes']
    if not 32 <= size <= 0x1000000:
        raise ValueError('Kernel exceeds the existing read window')
    kernel, sidecar = image[KERNEL_AT:KERNEL_AT + size], image[SIDECAR_AT:SIDECAR_AT + count]
    for data, key in ((kernel, 'kernel_sha256'), (sidecar, 'sidecar_sha256'), (payload['loader.efi'], 'loader_sha256')):
        if digest(data) != manifest[key]:
            raise ValueError('Embedded component hash mismatch')
    arguments = payload['cmdline'].rstrip(b'\0').decode().split()
    if any(a.startswith(('fairplay_native=', 'fplay_native=')) and a not in ('fairplay_native=0', 'fplay_native=0') for a in arguments):
        raise ValueError('Layout trial must not enable native FairPlay')
    if b'redwood_native_fairplay_gate_v1' not in kernel:
        raise ValueError('Default-off kernel gate missing')
    runtime_header = Path(__file__).resolve().parents[2] / 'xnuloader/native_sidecar.h'
    if digest(runtime_header.read_bytes()) != manifest['validator_sha256']:
        raise ValueError('Validated loader header differs from current source')
    validate(kernel, sidecar)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    verify(args.image, args.manifest)
    print('Layout-trial preflight passed: whole RECOVERY image required; BOOT is not written.')

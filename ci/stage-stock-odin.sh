#!/bin/bash
# Stage shared SM-G980F inputs from a checksum-pinned stock AP package.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ ${STOCK_AP_URL:-} == https://* && ${STOCK_AP_SHA256:-} =~ ^[0-9a-f]{64}$ ]] || {
    echo 'Configure an HTTPS stock AP (or Odin ZIP) URL and its SHA-256.' >&2
    exit 1
}

tmp=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/jesvi-odin.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
curl --fail --silent --show-error --location --retry 3 --proto '=https' --proto-redir '=https' \
    -o "$tmp/odin" "$STOCK_AP_URL"
printf '%s  %s\n' "$STOCK_AP_SHA256" "$tmp/odin" | shasum -a 256 -c -
python3 - "$tmp" <<'PYTHON'
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

directory = Path(sys.argv[1])
download = directory / 'odin'
ap = download
if zipfile.is_zipfile(download):
    with zipfile.ZipFile(download) as archive:
        matches = [entry for entry in archive.namelist()
                   if Path(entry).name.startswith('AP_G980F') and entry.endswith('.tar.md5')]
        if len(matches) != 1:
            raise SystemExit('Expected exactly one SM-G980F AP archive in the Odin ZIP')
        ap = directory / 'ap.tar'
        with archive.open(matches[0]) as source, ap.open('wb') as target:
            shutil.copyfileobj(source, target)
    download.unlink()
with tarfile.open(ap, 'r:') as archive:
    entries = archive.getmembers()
    for image in ('boot', 'super'):
        matches = [entry for entry in entries if entry.isfile()
                   and entry.name.removeprefix('./') == image + '.img.lz4']
        if len(matches) != 1:
            raise SystemExit(f'Expected one {image}.img.lz4 in SM-G980F AP')
        with archive.extractfile(matches[0]) as source, (directory / f'{image}.lz4').open('wb') as target:
            shutil.copyfileobj(source, target)
ap.unlink()
PYTHON
for image in boot super; do
    lz4 -d "$tmp/$image.lz4" "$tmp/$image.img"
    rm "$tmp/$image.lz4"
done

# The S20 uses a non-A/B vendor partition in its stock super image.
curl --fail --silent --show-error --location --retry 3 --proto '=https' --proto-redir '=https' \
    -o "$tmp/lpunpack.py" \
    https://raw.githubusercontent.com/unix3dgforce/lpunpack/c59b8f3b069c5a8aa438a049fa4a091177172434/lpunpack.py
mkdir "$tmp/partitions"
python3 "$tmp/lpunpack.py" -p vendor "$tmp/super.img" "$tmp/partitions"
rm "$tmp/super.img"
7z x -y "-o$tmp/vendor" "$tmp/partitions/vendor.img" 'firmware/*' 'tee/*'
test -d "$tmp/vendor/firmware"
test -d "$tmp/vendor/tee"
find "$tmp/vendor/tee" -type f -name '*46494e474502' | grep -q .
test -s fw-sensors/bcm_sensorhub_a1.bin

python3 - "$tmp/boot.img" <<'PYTHON'
import struct
import sys
from pathlib import Path

sys.path.insert(0, 'tools/release')
from images import android_head

boot = Path(sys.argv[1])
android_head(boot)
with boot.open('rb') as source:
    header = source.read(2048)
    def word(offset):
        return struct.unpack_from('<I', header, offset)[0]

    offset = 2048 + sum((word(i) + 2047) // 2048 * 2048 for i in (8, 16, 24, 1632))
    source.seek(offset)
    table = source.read(word(1648))
output = Path('inputs/x1slte/boot-dtb.img')
with output.open('xb') as target:
    target.write(table)
PYTHON

bash tools/s20-firmware.sh "$tmp/vendor/firmware"
mkdir fw-fingerprint
cp -R "$tmp/vendor/tee/." fw-fingerprint/
echo 'Prepared SM-G980F stock DT table, Wi-Fi, audio, haptic and fingerprint firmware.'

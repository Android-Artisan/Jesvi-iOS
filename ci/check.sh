#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/release/manage.py validate
python3 -m unittest discover -s tools/release/tests -v
python3 tools/passcode/test_build_integration.py
python3 tools/passcode/test_restore_files.py
python3 tools/passcode/test_native_adapter.py
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
cc -std=c99 -Wall -Wextra -Werror tools/release/tests/data_volume_test.c -o "$test_dir/data-volume"
"$test_dir/data-volume"
for file in build.sh run.sh ci/*.sh x1slte.sh userland/build.sh userland/build-gpu-y2s.sh xnu-deps/setup.sh; do
    bash -n "$file"
done
git diff --check

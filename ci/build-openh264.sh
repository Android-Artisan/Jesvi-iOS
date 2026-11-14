#!/bin/bash
# Isolated pinned source build; never use a library from another agent's tree.
set -euo pipefail
cd "$(dirname "$0")/.."
set +u # Legacy environment setup reads optional unset variables.
source ./xnu-env.sh
set -u
python3 - <<'PY'
import json, pathlib, subprocess
root = pathlib.Path.cwd()
lock = json.loads((root/'ci/sources.lock.json').read_text())['sources']['openh264']
source = root/'.cache/release-deps/openh264-source'
if not source.exists():
    source.mkdir(parents=True)
    subprocess.run(['git','init','-q',str(source)],check=True)
    subprocess.run(['git','-C',str(source),'fetch','--depth=1',lock['url'],lock['commit']],check=True)
    subprocess.run(['git','-C',str(source),'checkout','--detach',lock['commit']],check=True)
head = subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
if head != lock['commit'] or subprocess.check_output(['git','-C',str(source),'status','--porcelain','--untracked-files=no']):
    raise SystemExit('OpenH264 source differs from reviewed lock')
PY
# OpenH264 probes xcrun with --sdk, which the kernel's Linux shim does not support.
# Keep native host tools first while passing the reviewed iOS SDK explicitly.
PATH="/usr/bin:/bin:$PATH" make -C .cache/release-deps/openh264-source -j4 OS=ios ARCH=arm64 SDK_MIN=16.0 SDKROOT="$THEOS/sdks/iPhoneOS16.5.sdk" libraries

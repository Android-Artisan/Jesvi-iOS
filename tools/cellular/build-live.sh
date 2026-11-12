#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/out/cellular/live"
sdk=${SDK:-"$HOME/theos/sdks/iPhoneOS16.5.sdk"}
mkdir -p "$out"
xcrun clang -target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -Wextra -Werror \
    "$repo/tools/cellular/live-pcie.c" -o "$out/live-pcie"
codesign --force --sign - --timestamp=none "$out/live-pcie"
codesign --verify --strict "$out/live-pcie"
printf '%s\n' "$out/live-pcie"

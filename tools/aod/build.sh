#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
sdk="${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk"
output="$PWD/out/aod"
mkdir -p "$output"
cc=$(/usr/bin/xcrun --find clang)
flags=(-target arm64-apple-ios16.0 -isysroot "$sdk" -fobjc-arc -fblocks -O2 -Wall -Wextra -Werror)
"$cc" "${flags[@]}" -dynamiclib -framework Foundation -framework UIKit \
  -Wl,-install_name,/var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.dylib \
  tools/aod/preview.m -o "$output/RedwoodAODPreview.dylib"
"$cc" "${flags[@]}" -framework Foundation tools/aod/control.m -o "$output/aod-preview"
/usr/bin/codesign -f -s - "$output/RedwoodAODPreview.dylib"
/usr/bin/codesign -f -s - "$output/aod-preview"
echo "Built appearance-only test. Nothing installed or restarted."

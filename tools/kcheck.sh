#!/bin/zsh
# kcheck.sh FILE...: syntax-check kernel C files with the build's own flags (no build, no shared state touched)
X=/Users/jesvi/ios-port-research/work/hackios-xnu-kernel/xnu-src/xnu-8796.121.2
C=$(cat /Users/jesvi/ios-port-research/work/hackios-xnu-kernel/proposals/ufs-async/.cc-cmd.txt); C=${C/ -c / -fsyntax-only -Wall -I$X/bsd/dev }
C=$(echo "$C" | sed -E 's/ [^ ]*redwood_ufs\.c( |$)/ /; s/ -o [^ ]+//; s/ -MD -MF [^ ]+//')
export PATH=/Users/jesvi/ios-port-research/work/hackios-xnu-kernel/xnu-deps/host-macos/bin:$PATH
cd $X/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT
for f in "$@"; do n=$(eval "$C $f" 2>&1 | grep -cE "error|warning:"); echo "$(basename $f): $n errors/warnings"; eval "$C $f" 2>&1 | grep -E "error|warning:" | head -5; done

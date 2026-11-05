#!/bin/zsh
# x1slte-auto.sh [system.apfs]   hands-free system-image update while iOS runs on Wi-Fi (uroot)
# x1slte-auto.sh --recovery      the same for out/x1slte-recovery.img (kernel, loader, root.apfs: uflash)
# logs saved -> phone to Log access (rwlogd /menu?to=1) -> uroot -> cold restart -> wait for iOS on Wi-Fi again.
cd "$(dirname "$0")/.."
IP=${Y2S_IP:-192.168.137.106}; F=${1:-userland/system.apfs}; T=$(date +%Y%m%d-%H%M%S); D=logs/runs/$T; mkdir -p $D
up() { [ "$(curl -s -m 4 -o /dev/null -w '%{http_code}' http://$IP:8080/)" = 200 ]; }
wake() { ping -c1 -t1 $IP >/dev/null 2>&1; }
echo "[auto] phone at $IP"; for i in $(seq 1 20); do wake; up && break; sleep 3; done; up || { echo "[auto] phone not on Wi-Fi"; exit 1; }
curl -s -m 60 http://$IP:8080/m2m > $D/m2m.txt; curl -s -m 90 http://$IP:8080/log > $D/kernel.txt
echo "[auto] logs saved: $D ($(wc -l < $D/m2m.txt) m2m records, $(wc -c < $D/kernel.txt) bytes of kernel log)"
echo "[auto] asking the phone for Log access"; curl -s -m 10 "http://$IP:8080/menu?to=1"; echo
for i in $(seq 1 60); do fastboot devices 2>/dev/null | grep -q fastboot && break; sleep 2; done
fastboot devices | grep -q fastboot || { echo "[auto] the phone did not reach Log access"; exit 1; }
./x1slte.sh ulog > $D/lastlog.txt 2>&1 || true
if [ "$F" = --recovery ]; then ./x1slte.sh uflash || exit 1; else ./x1slte.sh uroot "$F" || exit 1; fi
./x1slte.sh ureboot
echo "[auto] waiting for iOS on Wi-Fi"; for i in $(seq 1 90); do sleep 4; wake; up && { echo "[auto] iOS is back on Wi-Fi after $((i*4)) s"; exit 0; }; done
echo "[auto] iOS did not come back on Wi-Fi within 6 min"; exit 2

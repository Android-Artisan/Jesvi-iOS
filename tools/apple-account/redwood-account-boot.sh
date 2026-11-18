#!/var/jb/bin/sh
# Re-apply the Store account injection rules at boot so App Store / Media & Purchases
# login keeps working after a reboot. The dylibs live in stable /var/jb/usr/lib.
# The owner Mac must be running the broker + USB tunnel for sign-in to actually work;
# this only restores the phone-side hooks. Idempotent.
LOGDIR=/var/mobile/Library/RedwoodAppleAccountLab
mkdir -p "$LOGDIR" 2>/dev/null
echo "=== account-store boot $(date) ==="
AL=/var/jb/usr/lib/libredwood-account-layer-9b41e2a7df42db8b.dylib
MO=/var/jb/usr/lib/libredwood-metaloff.dylib
SB=/var/jb/usr/lib/libredwood-store-build.dylib
RC=/var/jb/usr/lib/libredwood-media-recorder.dylib
# wait for kernel rwsys + the broker connection config
i=0
while [ $i -lt 90 ]; do
  /redwood/bin/rwsys kern.exec_inject >/dev/null 2>&1 && break
  i=$((i+1)); sleep 1
done
for f in "$AL" "$SB"; do [ -f "$f" ] || { echo "missing $f, abort"; exit 1; }; done
cur=$(/redwood/bin/rwsys kern.exec_inject)
case "$cur" in *libredwood-store-build*) echo "rules already present; nothing to do"; exit 0;; esac
new="$cur;$MO Preferences;$AL akd accountsd appleaccountd amsaccountsd appstored itunesstored storeaccountd Preferences AppStore;$SB accountsd amsaccountsd appstored itunesstored Preferences AppStore;$RC accountsd Preferences AppStore"
/redwood/bin/rwsys kern.exec_inject "$new" >/dev/null
if [ "$(/redwood/bin/rwsys kern.exec_inject)" = "$new" ]; then echo "rules applied"; else echo "rules MISMATCH"; exit 1; fi
for s in akd accountsd appleaccountd amsaccountsd appstored itunesstored; do
  for d in user/501 system; do j=$d/com.apple.$s
    if /var/jb/bin/launchctl print "$j" >/dev/null 2>&1; then /var/jb/bin/launchctl kickstart -k "$j" >/dev/null 2>&1; break; fi
  done
done
echo "account services restarted; boot setup done"

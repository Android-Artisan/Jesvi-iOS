#!/bin/bash
# install.sh: RedwoodPhone.app to /var/jb/Applications, registered (uicache), run by launchd as mobile.
set -e
cd "$(dirname "$0")"
scp -q -O -r out/RedwoodPhone.app s20:/var/tmp/RedwoodPhone.app.new
scp -q -O com.redwood.phone.plist s20:/var/tmp/com.redwood.phone.plist
ssh s20 'launchctl bootout system/com.redwood.phone 2>/dev/null; rm -rf /var/jb/Applications/RedwoodPhone.app &&
  mv /var/tmp/RedwoodPhone.app.new /var/jb/Applications/RedwoodPhone.app && chown -R root:wheel /var/jb/Applications/RedwoodPhone.app &&
  /var/jb/usr/bin/uicache -p /var/jb/Applications/RedwoodPhone.app &&
  mkdir -p /var/jb/redwood && mv /var/tmp/com.redwood.phone.plist /var/jb/redwood/ && chown root:wheel /var/jb/redwood/com.redwood.phone.plist &&
  launchctl bootstrap system /var/jb/redwood/com.redwood.phone.plist && sleep 2 && launchctl list | grep com.redwood.phone;
  tail -3 /var/mobile/Library/Logs/redwood-phone.log'

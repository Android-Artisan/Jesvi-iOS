#!/bin/zsh
# tee-up.sh: on a booted phone with the TEEGRIS kernel -- start TEEGRIS, the storage daemon, the driver TAs (as
# tzts_daemon does), then try the fingerprint TA. Prints the driver's status and the secure world's errors.
S="/var/jb/usr/sbin/sysctl"
ssh s20 "ps -A -o comm= | grep -q teetsd || (nohup /var/root/teetsd >/dev/null 2>&1 &); $S -w kern.redwood.tee=start >/dev/null 2>&1; sync; sleep 6
for f in /var/root/tee/driver/*; do $S -w kern.redwood.tee=\"openkeep \$f\" >/dev/null; sleep 3; $S kern.redwood.tee | tail -2 | head -1; done
$S -w kern.redwood.tee='open /var/root/tee/00000000-0000-0000-0000-46494e474502' >/dev/null; sleep 12
$S kern.redwood.tee; echo ---; tail -25 /var/root/teetsd.log"
curl -s -m 20 http://10.77.0.2:8080/log | grep -E 'redwood-tee|SW> .*(ERR|WRN|ACSD|driver|FINGE|finger)' | tail -40 | cut -c14-230

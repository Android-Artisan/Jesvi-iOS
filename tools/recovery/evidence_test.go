// SPDX-License-Identifier: MIT
package main

import (
	"testing"
)

func TestDeviceEvidenceRejectsWrongOrMissingIdentity(t *testing.T) {
	c := Config{Models: []string{"SM-G980F"}, Bootloaders: []string{"G980FXXSNHYB7"}}
	base := []string{"SM-G980F", "G980FXXSNHYB7", "x1s", "exynos990", "0", "unlocked"}
	check := func(v []string) error { return verifyIdentity(c, v[0], v[1], v[2], v[3], v[4], v[5]) }
	if e := check(base); e != nil {
		t.Fatal(e)
	}
	for i := range base {
		v := append([]string(nil), base...)
		v[i] = "wrong"
		if e := check(v); e == nil {
			t.Fatal("bad identity evidence accepted", i)
		}
	}
	v := append([]string(nil), base...)
	v[4] = ""
	v[5] = ""
	if e := check(v); e == nil {
		t.Fatal("missing unlock evidence accepted")
	}
	v[4] = "1"
	v[5] = "unlocked"
	if e := check(v); e == nil {
		t.Fatal("conflicting unlock evidence accepted")
	}
}
func TestMountEvidenceIsParsedAndMissingEvidenceFails(t *testing.T) {
	mounted, e := mountedDevices("27 1 8:21 / /data rw,nosuid - ext4 /dev/block/sda21 rw\n28 1 253:0 / /system_root ro - ext4 /dev/block/dm-0 ro\n")
	if e != nil || !mounted["8:21"] || !mounted["253:0"] || mounted["8:1"] {
		t.Fatal(mounted, e)
	}
	for _, bad := range []string{"", "truncated", "27 1 bad / /data rw - ext4 /dev/block/sda21 rw", "27 1 8:999999999999999 / /data rw - ext4 /dev/block/sda21 rw"} {
		if _, e := mountedDevices(bad); e == nil {
			t.Fatal("missing/malformed mount evidence accepted")
		}
	}
}

func TestUntrustedErrorTextCannotInjectRecoveryCommands(t *testing.T) {
	if got := uiText("bad.zip\nwipe_cache\rreboot\x00"); got != "bad.zip wipe_cache reboot " {
		t.Fatal(got)
	}
}

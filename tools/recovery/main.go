// SPDX-License-Identifier: MIT
package main

import (
	"archive/zip"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
)

func uiText(s string) string {
	return strings.Map(func(r rune) rune {
		if r < 32 || r == 127 {
			return ' '
		}
		return r
	}, s)
}

var configSHA string // Bound by builder to exact metadata; no arbitrary destinations.
func run() error {
	args := os.Args
	if len(args) == 3 && args[1] == "--inspect" {
		z, e := zip.OpenReader(args[2])
		if e != nil {
			return e
		}
		defer z.Close()
		b, e := loadBundle(&z.Reader, configSHA)
		if e != nil {
			return e
		}
		fmt.Printf("Verified TWRP payload: action=%s device=%s mode=%s writes=%v hardware-qualified=false\n", b.Config.Action, b.Config.Device, b.Config.Mode, b.Config.AllowWrites)
		return nil
	}
	if len(args) != 4 || (args[1] != "3" && args[1] != "2") {
		return errors.New("expected recovery updater API, output FD and ZIP path; or --inspect ZIP")
	}
	fd, e := strconv.Atoi(args[2])
	if e != nil || fd < 3 {
		return errors.New("invalid recovery output FD")
	}
	pipe := os.NewFile(uintptr(fd), "recovery-output")
	if pipe == nil {
		return errors.New("recovery output unavailable")
	}
	logFile, e := os.OpenFile("/tmp/jesvi-install.log", os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if e != nil {
		return e
	}
	defer logFile.Close()
	log := func(s string) {
		fmt.Fprintln(logFile, s)
		fmt.Fprintln(os.Stderr, s)
		if _, e := fmt.Fprintf(pipe, "ui_print %s\nui_print\n", uiText(s)); e != nil {
			panic("recovery UI pipe failed; stopping installation")
		}
	}
	archive, e := os.Open(args[3])
	if e != nil {
		return e
	}
	defer archive.Close()
	stat, e := archive.Stat()
	if e != nil {
		return e
	}
	z, e := zip.NewReader(archive, stat.Size())
	if e != nil {
		return e
	}
	log("Verifying complete ZIP payload before any device writes...")
	b, e := loadBundle(z, configSHA)
	if e != nil {
		return e
	}
	if b.Config.Action == "restore" {
		log("Jesvi-iOS-Restore (" + b.Config.Device + "): back to Android. Hardware qualification pending.")
	} else {
		log("Jesvi iOS (" + b.Config.Device + "): EXPERIMENTAL installer. Hardware qualification pending.")
		log("Plan: reuse existing physical partitions; no partition-table changes.")
	}
	d, e := openRecovery(b.Config, archive)
	if e != nil {
		return e
	}
	defer d.Close()
	log("Exact device, bootloader, partition sizes and battery checks passed.")
	if b.Config.Action == "restore" {
		head, e := d.readHead("super", lpRegion)
		if e != nil {
			return e
		}
		if _, e = restoreState(head); e != nil {
			return e
		}
	}
	held, e := d.plan()
	if e != nil {
		return e
	}
	if !held.empty() {
		log("Android ROM storage is in use; after confirmation the installer will:")
		for _, s := range held.describe() {
			log("  " + s)
		}
	}
	if !b.Config.AllowWrites {
		log("PREFLIGHT ONLY: no partitions opened for writing. No reboot.")
		return nil
	}
	if b.Config.Action == "restore" {
		log("RESTORE: puts back Android's partition map and clears iOS data headers.")
		log("ALL iOS DATA WILL BE LOST. BOOT and TWRP stay as they are.")
		if e = confirmKeys("Erase iOS and prepare the phone for an Android ROM?", log); e != nil {
			return e
		}
		if e = d.release(log); e != nil {
			return e
		}
		if e = restoreAndroid(b, d, log); e != nil {
			return e
		}
		log("Done. Next, without rebooting: Wipe > Format Data (type yes),")
		log("then flash your Android ROM ZIP (ExtremeROM, UN1CA, ...) or use Odin with stock firmware.")
		return nil
	}
	valid, recognizable, e := d.dataState()
	if e != nil {
		return e
	}
	mode := b.Config.Mode
	if mode == "choose" {
		if valid {
			mode = "update"
		} else if recognizable {
			return errors.New("existing HFS header is damaged; refusing automatic fresh install")
		} else {
			mode = "fresh"
		}
	}
	if mode == "update" {
		if e = d.CheckData(); e != nil {
			return e
		}
		log("UPDATE: existing HFSX data/keybags will be preserved.")
	} else {
		log("FRESH INSTALL: ALL ANDROID/iOS USERDATA WILL BE LOST. This is not a secure erase.")
	}
	log("Use verified OFF-DEVICE backups and tested Download Mode rescue. TWRP will be overwritten.")
	if e = confirmKeys("Confirm backups/rescue and installation of this experimental candidate.", log); e != nil {
		return e
	}
	if mode == "fresh" {
		if e = confirmKeys("SECOND CONFIRMATION: erase userdata and initialize HFSX?", log); e != nil {
			return e
		}
	}
	if e = d.release(log); e != nil {
		return e
	}
	if e = d.CheckSafe(); e != nil {
		return e
	}
	if e = perform(b, d, mode, log); e != nil {
		return e
	}
	log("Installation readback completed. Keep this log off-device; boot testing is still required.")
	return nil
}
func reportFailure(message string) {
	fmt.Fprintln(os.Stderr, "INSTALL STOPPED:", message)
	if len(os.Args) == 4 && (os.Args[1] == "2" || os.Args[1] == "3") {
		if f, e := os.OpenFile("/tmp/jesvi-install.log", os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600); e == nil {
			fmt.Fprintln(f, "INSTALL STOPPED:", message)
			f.Close()
		}
		if fd, e := strconv.Atoi(os.Args[2]); e == nil && fd >= 3 {
			if p := os.NewFile(uintptr(fd), "recovery-output"); p != nil {
				fmt.Fprintf(p, "ui_print INSTALL STOPPED: %s\nui_print\n", uiText(message))
			}
		}
	}
}
func main() {
	defer func() {
		if r := recover(); r != nil {
			reportFailure(fmt.Sprint(r))
			os.Exit(1)
		}
	}()
	if e := run(); e != nil {
		reportFailure(e.Error())
		os.Exit(1)
	}
}

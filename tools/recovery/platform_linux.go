//go:build linux

// SPDX-License-Identifier: MIT
package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

type partition struct {
	Path, Identity string
	Rdev           uint64
	Size           int64
	Read           *os.File
}
type linuxDevice struct {
	dataLocked bool
	parts      map[string]partition
	writes     map[string]*os.File
	lock       *os.File
	archive    *os.File
}

func deviceID(dev uint64) string {
	major := (dev>>8)&0xfff | (dev>>32)&0xfffff000
	minor := dev&0xff | (dev>>12)&0xffffff00
	return fmt.Sprintf("%d:%d", major, minor)
}
func prop(key string) string {
	for _, p := range []string{"/sbin/getprop", "/system/bin/getprop", "/bin/getprop"} {
		if _, e := os.Stat(p); e == nil {
			v, e := exec.Command(p, key).Output()
			if e == nil {
				return strings.TrimSpace(string(v))
			}
		}
	}
	return ""
}
func openRecovery(c Config, archive *os.File) (*linuxDevice, error) {
	if runtime.GOARCH != "arm64" || os.Geteuid() != 0 || prop("ro.twrp.version") == "" {
		return nil, errors.New("requires arm64 root TWRP")
	}
	if e := verifyIdentity(c, prop("ro.product.model"), prop("ro.boot.bootloader"), prop("ro.product.device"), prop("ro.board.platform"), prop("ro.boot.flash.locked"), prop("ro.boot.vbmeta.device_state")); e != nil {
		return nil, e
	}
	readInt := func(path string) (int, error) {
		b, e := os.ReadFile(path)
		if e != nil {
			return 0, e
		}
		return strconv.Atoi(strings.TrimSpace(string(b)))
	}
	level, e := readInt("/sys/class/power_supply/battery/capacity")
	if e != nil || level < 50 || level > 100 {
		return nil, errors.New("battery must report at least 50 percent")
	}
	temp, e := readInt("/sys/class/power_supply/battery/temp")
	if e != nil || temp < 0 || temp > 450 {
		return nil, errors.New("battery temperature missing or outside 0..45 C")
	}
	fd, e := syscall.Open("/tmp/jesvi-install.lock", syscall.O_CREAT|syscall.O_RDWR|syscall.O_CLOEXEC|syscall.O_NOFOLLOW, 0600)
	if e != nil {
		return nil, e
	}
	lock := os.NewFile(uintptr(fd), "installer-lock")
	if e = syscall.Flock(fd, syscall.LOCK_EX|syscall.LOCK_NB); e != nil {
		lock.Close()
		return nil, errors.New("another installer owns storage")
	}
	d := &linuxDevice{parts: make(map[string]partition), writes: make(map[string]*os.File), lock: lock, archive: archive}
	good := false
	defer func() {
		if !good {
			d.Close()
		}
	}()
	dirs := []string{"/dev/block/by-name"}
	for _, pattern := range []string{"/dev/block/platform/*/by-name", "/dev/block/platform/*/*/by-name"} {
		found, e := filepath.Glob(pattern)
		if e != nil {
			return nil, e
		}
		dirs = append(dirs, found...)
	}
	seen := make(map[uint64]bool)
	for name, size := range capacities {
		var real string
		for _, dir := range dirs {
			p, e := filepath.EvalSymlinks(filepath.Join(dir, name))
			if e != nil {
				continue
			}
			if real != "" && real != p {
				return nil, fmt.Errorf("ambiguous %s partition aliases", name)
			}
			real = p
		}
		if real == "" {
			return nil, fmt.Errorf("partition by-name link missing: %s", name)
		}
		file, e := openNode(real, syscall.O_RDONLY)
		if e != nil {
			return nil, e
		}
		p, e := inspectNode(file, real, name, size)
		if e != nil {
			file.Close()
			return nil, e
		}
		if seen[p.Rdev] {
			file.Close()
			return nil, errors.New("duplicate target devices")
		}
		seen[p.Rdev] = true
		p.Read = file
		d.parts[name] = p
	}
	if e = d.CheckSafe(); e != nil {
		return nil, e
	}
	good = true
	return d, nil
}
func openNode(path string, flags int) (*os.File, error) {
	fd, e := syscall.Open(path, flags|syscall.O_CLOEXEC|syscall.O_NOFOLLOW, 0)
	if e != nil {
		return nil, e
	}
	return os.NewFile(uintptr(fd), path), nil
}
func inspectNode(f *os.File, path, name string, size int64) (partition, error) {
	st, e := f.Stat()
	if e != nil {
		return partition{}, e
	}
	if st.Mode()&os.ModeDevice == 0 || st.Mode()&os.ModeCharDevice != 0 {
		return partition{}, errors.New("target is not a block device")
	}
	dev := st.Sys().(*syscall.Stat_t).Rdev
	id := deviceID(dev)
	var bytes uint64
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), 0x80081272, uintptr(unsafe.Pointer(&bytes))) // BLKGETSIZE64
	if errno != 0 || bytes != uint64(size) {
		return partition{}, fmt.Errorf("%s partition capacity differs from measured profile", name)
	}
	sys := filepath.Join("/sys/dev/block", id)
	if _, e = os.Stat(filepath.Join(sys, "partition")); e != nil {
		return partition{}, errors.New("target is not a physical partition")
	}
	u, e := os.ReadFile(filepath.Join(sys, "uevent"))
	if e != nil {
		return partition{}, e
	}
	found := false
	for _, line := range strings.Split(string(u), "\n") {
		if strings.HasPrefix(line, "PARTNAME=") && strings.EqualFold(strings.TrimPrefix(line, "PARTNAME="), name) {
			found = true
		}
	}
	if !found {
		return partition{}, fmt.Errorf("%s partition label does not match kernel evidence", name)
	}
	return partition{Path: path, Identity: id, Rdev: dev, Size: size}, nil
}
func (d *linuxDevice) CheckSafe() error {
	text, e := os.ReadFile("/proc/self/mountinfo")
	if e != nil {
		return e
	}
	mounted, e := mountedDevices(string(text))
	if e != nil {
		return e
	}
	st, e := d.archive.Stat()
	if e != nil || !st.Mode().IsRegular() {
		return errors.New("package must be a seekable regular file")
	}
	sourceID := deviceID(st.Sys().(*syscall.Stat_t).Dev)
	for name, p := range d.parts {
		if mounted[p.Identity] || sourceID == p.Identity {
			return fmt.Errorf("%s is mounted or holds the ZIP; use external storage/sideload and unmount targets", name)
		}
		holders, e := os.ReadDir(filepath.Join("/sys/dev/block", p.Identity, "holders"))
		if e != nil {
			return e
		}
		if len(holders) > 0 {
			return fmt.Errorf("%s has device-mapper/other holders; release them in recovery before installation", name)
		}
		info, e := os.Stat(p.Path)
		if e != nil {
			return e
		}
		if info.Sys().(*syscall.Stat_t).Rdev != p.Rdev {
			return errors.New("device node identity changed")
		}
	}
	return nil
}
func (d *linuxDevice) OpenWritable(name string) (Target, error) {
	p, ok := d.parts[name]
	if !ok {
		return nil, errors.New("protected/unknown partition destination")
	}
	if f := d.writes[name]; f != nil {
		return blockTarget{f}, nil
	}
	f, e := openNode(p.Path, syscall.O_RDWR|syscall.O_EXCL)
	if e != nil {
		return nil, e
	}
	q, e := inspectNode(f, p.Path, name, p.Size)
	if e != nil || q.Rdev != p.Rdev {
		f.Close()
		return nil, errors.New("exclusive target open/identity verification failed")
	}
	d.writes[name] = f
	return blockTarget{f}, nil
}
func (d *linuxDevice) CheckData() error {
	if !d.dataLocked {
		p := d.parts["userdata"]
		f, e := openNode(p.Path, syscall.O_RDONLY|syscall.O_EXCL)
		if e != nil {
			return e
		}
		checked, e := inspectNode(f, p.Path, "userdata", p.Size)
		if e != nil || checked.Rdev != p.Rdev {
			f.Close()
			return errors.New("userdata read-only lock/identity verification failed")
		}
		p.Read.Close()
		p.Read = f
		d.parts["userdata"] = p
		d.dataLocked = true
	}
	if !initialized(d.parts["userdata"].Read, capacities["userdata"]) {
		return errors.New("existing HFSX userdata is missing/damaged; update will not format it")
	}
	return nil
}
func (d *linuxDevice) dataState() (bool, bool, error) {
	h := make([]byte, 512)
	if _, e := d.parts["userdata"].Read.ReadAt(h, 1024); e != nil {
		return false, false, e
	}
	return validHFS(h, capacities["userdata"]), string(h[:2]) == "HX" || string(h[:2]) == "H+", nil
}
func (d *linuxDevice) Close() error {
	for _, f := range d.writes {
		f.Close()
	}
	for _, p := range d.parts {
		if p.Read != nil {
			p.Read.Close()
		}
	}
	if d.lock != nil {
		d.lock.Close()
	}
	return nil
}

// A new physical key-down held for three seconds authorizes each destructive mode.
func confirmKeys(prompt string, log func(string)) error {
	log(prompt)
	log("Hold VOLUME UP for 3 seconds to confirm; VOLUME DOWN/POWER cancels. Timeout: 45 seconds.")
	paths, e := filepath.Glob("/dev/input/event*")
	if e != nil || len(paths) == 0 {
		return errors.New("no physical key input devices")
	}
	events := make(chan [2]int, 32)
	done := make(chan struct{})
	defer close(done)
	var files []*os.File
	defer func() {
		for _, f := range files {
			f.Close()
		}
	}()
	for _, path := range paths {
		f, e := os.Open(path)
		if e != nil {
			continue
		}
		files = append(files, f)
		go func(f *os.File) {
			for {
				var raw [24]byte
				if _, e := io.ReadFull(f, raw[:]); e != nil {
					return
				}
				if binary.LittleEndian.Uint16(raw[16:18]) != 1 {
					continue
				}
				event := [2]int{int(binary.LittleEndian.Uint16(raw[18:20])), int(int32(binary.LittleEndian.Uint32(raw[20:24])))}
				select {
				case events <- event:
				case <-done:
					return
				}
			}
		}(f)
	}
	if len(files) == 0 {
		return errors.New("cannot read physical key input")
	}
	timeout := time.NewTimer(45 * time.Second)
	defer timeout.Stop()
	var hold *time.Timer
	var held <-chan time.Time
	defer func() {
		if hold != nil {
			hold.Stop()
		}
	}()
	for {
		select {
		case <-timeout.C:
			return errors.New("physical confirmation timed out; no writes authorized")
		case ev := <-events:
			if (ev[0] == 114 || ev[0] == 116) && ev[1] == 1 {
				return errors.New("installation cancelled")
			}
			if ev[0] == 115 {
				if ev[1] == 1 {
					if hold != nil {
						hold.Stop()
					}
					hold = time.NewTimer(3 * time.Second)
					held = hold.C
				}
				if ev[1] == 0 && hold != nil {
					hold.Stop()
					held = nil
				}
			}
		case <-held:
			return nil
		}
	}
}

// Flush dirty data and invalidate cached block pages before readback.
type blockTarget struct{ *os.File }

func (b blockTarget) Sync() error {
	if e := b.File.Sync(); e != nil {
		return e
	}
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, b.Fd(), 0x1261, 0) // BLKFLSBUF
	if errno != 0 {
		return errno
	}
	return nil
}

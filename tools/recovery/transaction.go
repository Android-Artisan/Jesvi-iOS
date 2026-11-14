// SPDX-License-Identifier: MIT
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
)

// The transaction core cannot name, resize or repartition any disk. The platform
// supplies four already-checked descriptors; tests supply independent files.
type Target interface {
	io.ReaderAt
	io.WriterAt
	Sync() error
}
type Device interface {
	OpenWritable(string) (Target, error)
	CheckSafe() error
	Close() error
}
type zeroReader struct{}

func (zeroReader) Read(p []byte) (int, error) { clear(p); return len(p), nil }
func transfer(target Target, input io.Reader, offset, length int64, want string) error {
	h := sha256.New()
	buf := make([]byte, 1<<20)
	at := int64(0)
	for at < length {
		take := int64(len(buf))
		if length-at < take {
			take = length - at
		}
		n, e := io.ReadFull(input, buf[:take])
		if e != nil {
			return fmt.Errorf("short source: %w", e)
		}
		written, e := target.WriteAt(buf[:n], offset+at)
		if e != nil {
			return e
		}
		if written != n {
			return io.ErrShortWrite
		}
		h.Write(buf[:n])
		at += int64(n)
	}
	// Even after prevalidation, detect changed/truncated sources during writing.
	if hex.EncodeToString(h.Sum(nil)) != want {
		return errors.New("source changed during write")
	}
	if e := target.Sync(); e != nil {
		return e
	}
	readback := sha256.New()
	if _, e := io.CopyN(readback, io.NewSectionReader(target, offset, length), length); e != nil {
		return e
	}
	if hex.EncodeToString(readback.Sum(nil)) != want {
		return errors.New("readback checksum mismatch")
	}
	return nil
}
func initialized(target io.ReaderAt, size int64) bool {
	h := make([]byte, 512)
	if _, e := target.ReadAt(h, 1024); e != nil {
		return false
	}
	return validHFS(h, size)
}
func perform(b *Bundle, device Device, mode string, log func(string)) error {
	if mode != "fresh" && mode != "update" {
		return errors.New("installation mode must be explicit")
	}
	extents, e := parseKit(b.Kit, capacities["userdata"])
	if e != nil {
		return e
	}
	if e = device.CheckSafe(); e != nil {
		return e
	}
	// Open all required descriptors exclusively before the first write.
	names := []string{"super", "recovery", "boot"}
	if mode == "fresh" {
		names = append(names, "userdata")
	}
	targets := make(map[string]Target)
	for _, name := range names {
		t, e := device.OpenWritable(name)
		if e != nil {
			return e
		}
		targets[name] = t
	}
	if mode == "update" {
		// Backend checked existing data read-only before confirmation; check again at commit time.
		if checker, ok := device.(interface{ CheckData() error }); !ok {
			return errors.New("missing update data check")
		} else if e = checker.CheckData(); e != nil {
			return e
		}
	}
	if e = device.CheckSafe(); e != nil {
		return e
	}
	writeImage := func(name string) error {
		if e := device.CheckSafe(); e != nil {
			return e
		}

		m := b.Config.Files[name]
		target := targets[m.Partition]
		if e := target.Sync(); e != nil {
			return e
		}
		existing := sha256.New()
		if _, e := io.CopyN(existing, io.NewSectionReader(target, m.Offset, m.Bytes), m.Bytes); e != nil {
			return e
		}
		if hex.EncodeToString(existing.Sum(nil)) == m.SHA256 {
			log("Already matches verified " + m.Partition + "; skipping write")
			return nil
		}
		var r io.ReadCloser
		var e error
		if data, ok := b.Staged[name]; ok {
			r = io.NopCloser(bytes.NewReader(data))
		} else {
			r, e = b.Files[name].Open()
		}
		if e != nil {
			return e
		}
		defer r.Close()
		log("Writing and verifying " + m.Partition)
		return transfer(targets[m.Partition], r, m.Offset, m.Bytes, m.SHA256)
	}
	if e = writeImage("system.apfs"); e != nil {
		return fmt.Errorf("system: %w", e)
	}
	if mode == "fresh" {
		log("Explicit fresh install: initializing HFSX userdata (not a secure erase)")
		for _, x := range extents {
			if e = device.CheckSafe(); e != nil {
				return e
			}
			var input io.Reader = io.LimitReader(zeroReader{}, x.Length)
			if x.Data != nil {
				input = bytes.NewReader(x.Data)
			}
			h := sha256.New()
			var again io.Reader = io.LimitReader(zeroReader{}, x.Length)
			if x.Data != nil {
				again = bytes.NewReader(x.Data)
			}
			if _, e = io.Copy(h, again); e != nil {
				return e
			}
			if e = transfer(targets["userdata"], input, x.Offset, x.Length, hex.EncodeToString(h.Sum(nil))); e != nil {
				return fmt.Errorf("userdata: %w", e)
			}
		}
		if !initialized(targets["userdata"], capacities["userdata"]) {
			return errors.New("new HFS geometry verification failed")
		}
	}
	if e = writeImage("recovery.img"); e != nil {
		return fmt.Errorf("recovery: %w", e)
	}
	if e = writeImage("boot.img"); e != nil {
		return fmt.Errorf("boot: %w", e)
	}
	log("All written ranges passed readback. No reboot requested.")
	return nil
}

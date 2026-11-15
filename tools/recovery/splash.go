// SPDX-License-Identifier: MIT
package main

// Samsung's power-on splash (A, 2026-11-15). up_param is a tar of the bootloader's JPEG pictures; logo.jpg is the
// one shown at power-on. The installer replaces only that member with the release's splash (iOS's boot logo, made at
// build time from the user's own iOS root) and keeps the phone's original as logo.jpg.stock in the same tar, so
// every other picture of that phone's bootloader version stays byte for byte, and Jesvi-iOS-Restore can put the
// original back without any other copy. Nothing here is written unless the whole new tar parses back to exactly
// the intended members.

import (
	"archive/tar"
	"bytes"
	"errors"
	"fmt"
	"io"
	"strings"
)

const (
	splashName  = "logo.jpg"
	splashStock = "logo.jpg.stock"
	splashMax   = 1 << 20
)

type tarMember struct {
	Header tar.Header
	Data   []byte
}

func parsePictures(img []byte) ([]tarMember, error) {
	r := tar.NewReader(bytes.NewReader(img))
	var out []tarMember
	seen := map[string]bool{}
	for {
		h, e := r.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return nil, fmt.Errorf("up_param is not a readable tar: %w", e)
		}
		if h.Typeflag != tar.TypeReg && h.Typeflag != tar.TypeRegA {
			return nil, fmt.Errorf("up_param holds a non-file member: %q", h.Name)
		}
		if h.Name == "" || strings.ContainsAny(h.Name, "/\\") || seen[h.Name] || h.Size < 0 || h.Size > 8<<20 {
			return nil, fmt.Errorf("up_param holds an unexpected member: %q", h.Name)
		}
		seen[h.Name] = true
		data, e := io.ReadAll(r)
		if e != nil {
			return nil, e
		}
		out = append(out, tarMember{Header: *h, Data: data})
	}
	if len(out) == 0 {
		return nil, errors.New("up_param holds no pictures")
	}
	return out, nil
}

func buildPictures(members []tarMember, size int) ([]byte, error) {
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	for _, m := range members {
		h := m.Header
		h.Size = int64(len(m.Data))
		h.Format = tar.FormatUSTAR
		h.Typeflag = tar.TypeReg
		if e := w.WriteHeader(&h); e != nil {
			return nil, e
		}
		if _, e := w.Write(m.Data); e != nil {
			return nil, e
		}
	}
	if e := w.Close(); e != nil {
		return nil, e
	}
	if buf.Len() > size {
		return nil, fmt.Errorf("new pictures (%d bytes) do not fit up_param (%d)", buf.Len(), size)
	}
	return append(buf.Bytes(), make([]byte, size-buf.Len())...), nil
}

func memberIndex(ms []tarMember, name string) int {
	for i, m := range ms {
		if m.Header.Name == name {
			return i
		}
	}
	return -1
}

// verifyPictures: the new image parses back to exactly want (names, order, contents)
func verifyPictures(img []byte, want []tarMember) error {
	got, e := parsePictures(img)
	if e != nil {
		return e
	}
	if len(got) != len(want) {
		return errors.New("rebuilt up_param has a different member list")
	}
	for i := range got {
		if got[i].Header.Name != want[i].Header.Name || !bytes.Equal(got[i].Data, want[i].Data) {
			return fmt.Errorf("rebuilt up_param differs at %q", want[i].Header.Name)
		}
	}
	return nil
}

// withSplash: up_param with logo.jpg = splash; the phone's own logo.jpg kept once as logo.jpg.stock.
// changed false: it already shows this splash.
func withSplash(img, splash []byte) ([]byte, bool, error) {
	if len(splash) == 0 || len(splash) > splashMax || !bytes.HasPrefix(splash, []byte{0xff, 0xd8}) {
		return nil, false, errors.New("splash is not a JPEG of at most 1 MiB")
	}
	ms, e := parsePictures(img)
	if e != nil {
		return nil, false, e
	}
	i := memberIndex(ms, splashName)
	if i < 0 {
		return nil, false, errors.New("up_param has no logo.jpg")
	}
	if bytes.Equal(ms[i].Data, splash) {
		return img, false, nil
	}
	if memberIndex(ms, splashStock) < 0 {
		stock := ms[i]
		stock.Header.Name = splashStock
		ms = append(ms, tarMember{Header: stock.Header, Data: append([]byte(nil), ms[i].Data...)})
	}
	ms[i].Data = splash
	out, e := buildPictures(ms, len(img))
	if e != nil {
		return nil, false, e
	}
	return out, true, verifyPictures(out, ms)
}

// withoutSplash: logo.jpg back from logo.jpg.stock, which is then removed. changed false: nothing saved to restore.
func withoutSplash(img []byte) ([]byte, bool, error) {
	ms, e := parsePictures(img)
	if e != nil {
		return nil, false, e
	}
	s, i := memberIndex(ms, splashStock), memberIndex(ms, splashName)
	if s < 0 {
		return img, false, nil
	}
	if i < 0 {
		return nil, false, errors.New("up_param has a saved logo but no logo.jpg")
	}
	ms[i].Data = ms[s].Data
	ms = append(ms[:s], ms[s+1:]...)
	out, e := buildPictures(ms, len(img))
	if e != nil {
		return nil, false, e
	}
	return out, true, verifyPictures(out, ms)
}

// applySplash / restoreSplash: read the whole partition, rewrite it, write it back with readback (transfer).
func applySplash(t Target, splash []byte, log func(string)) error {
	return rewritePictures(t, log, func(img []byte) ([]byte, bool, error) { return withSplash(img, splash) },
		"Power-on splash already set", "Setting the power-on splash (the phone's own logo kept for Jesvi-iOS-Restore)")
}
func restoreSplash(t Target, log func(string)) error {
	return rewritePictures(t, log, withoutSplash,
		"No saved power-on splash: Samsung's is in place", "Putting back the phone's own power-on splash")
}
func rewritePictures(t Target, log func(string), edit func([]byte) ([]byte, bool, error), same, doing string) error {
	size := capacities["up_param"]
	if size <= 0 {
		return errors.New("no up_param in the device profile")
	}
	img, e := readRange(t, 0, int(size))
	if e != nil {
		return e
	}
	out, changed, e := edit(img)
	if e != nil {
		return e
	}
	if !changed {
		log(same)
		return nil
	}
	log(doing)
	return transfer(t, bytes.NewReader(out), 0, size, checkedBytes(out))
}

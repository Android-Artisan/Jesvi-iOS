//go:build !linux

// SPDX-License-Identifier: MIT
package main

import (
	"errors"
	"os"
)

type linuxDevice struct{}

func openRecovery(Config, *os.File) (*linuxDevice, error) {
	return nil, errors.New("device installation requires Linux arm64 TWRP")
}
func (*linuxDevice) OpenWritable(string) (Target, error) {
	return nil, errors.New("no device backend on host")
}
func (*linuxDevice) CheckSafe() error { return errors.New("no device backend on host") }
func (*linuxDevice) CheckData() error { return errors.New("no device backend on host") }
func (*linuxDevice) dataState() (bool, bool, error) {
	return false, false, errors.New("no device backend on host")
}
func (*linuxDevice) Close() error            { return nil }
func confirmKeys(string, func(string)) error { return errors.New("physical keys unavailable on host") }

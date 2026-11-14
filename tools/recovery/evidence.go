// SPDX-License-Identifier: MIT
package main

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

func mountedDevices(text string) (map[string]bool, error) {
	mounted := make(map[string]bool)
	lines := strings.Split(strings.TrimSpace(text), "\n")
	for _, line := range lines {
		fields := strings.Fields(line)
		if len(fields) < 10 || !strings.Contains(line, " - ") {
			return nil, errors.New("malformed/missing mount evidence")
		}
		id := strings.Split(fields[2], ":")
		if len(id) != 2 {
			return nil, errors.New("malformed mount device identity")
		}
		for _, part := range id {
			if _, e := strconv.ParseUint(part, 10, 32); e != nil {
				return nil, errors.New("malformed mount device identity")
			}
		}
		mounted[fields[2]] = true
	}
	return mounted, nil
}
func verifyIdentity(c Config, model, bootloader, product, soc, locked, state string) error {
	if !hasModel(c.Models, model) || !hasModel(c.Bootloaders, bootloader) {
		return fmt.Errorf("exact model/bootloader mismatch: model=%q bootloader=%q", model, bootloader)
	}
	if product != "x1s" && product != "x1slte" {
		return fmt.Errorf("unqualified recovery product: %s", product)
	}
	if soc != "exynos990" && soc != "exynos9830" {
		return fmt.Errorf("unqualified SoC property: %s", soc)
	}
	if (locked != "0" && state != "unlocked") || (locked != "" && locked != "0") || (state != "" && state != "unlocked") {
		return errors.New("unlocked bootloader evidence missing/conflicting")
	}
	return nil
}

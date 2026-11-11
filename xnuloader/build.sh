#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"

# On macOS the system clang cannot target a PE/COFF AArch64 image, and LLVM 23 no longer
# bundles the linker: Homebrew LLVM provides clang, the separate `lld` formula provides
# lld-link. On Linux clang/lld-link are used as they are.
#   brew install llvm lld
CC=${CC:-}
LD=${LD:-}
if [ "$(uname -s)" = Darwin ]; then
	LLVM=$(brew --prefix llvm 2>/dev/null || true)
	if [ -z "$CC" ] && [ -x "$LLVM/bin/clang" ]; then
		CC="$LLVM/bin/clang"
	fi
	if [ -z "$LD" ]; then
		if [ -x "$(brew --prefix lld 2>/dev/null)/bin/lld-link" ]; then
			LD="$(brew --prefix lld)/bin/lld-link"
		else
			LD=lld-link
		fi
	fi
fi
CC=${CC:-clang}
LD=${LD:-lld-link}
if [ -n "${CELLULAR_MMIO_PREFLIGHT:-}" ] && [ -z "${CELLULAR_PREFLIGHT_ONLY:-}" ]; then
	echo 'CELLULAR_MMIO_PREFLIGHT requires the original-loader preflight build' >&2
	exit 1
fi
if [ -n "${CELLULAR_SPI_LOOPBACK:-}" ] && { [ -z "${CELLULAR_MMIO_PREFLIGHT:-}" ] || [ -z "${CELLULAR_PREFLIGHT_ONLY:-}" ] || [ -z "${CELLULAR_RESERVE_CP:-}" ]; }; then
	echo 'CELLULAR_SPI_LOOPBACK requires MMIO + original-loader preflight + CP reservations' >&2
	exit 1
fi

"$CC" --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter ${CELLULAR_SPI_LOOPBACK:+-DCELLULAR_SPI_LOOPBACK=1} ${CELLULAR_MMIO_PREFLIGHT:+-DCELLULAR_MMIO_PREFLIGHT=1} ${CELLULAR_RESERVE_CP:+-DCELLULAR_RESERVE_CP=1} ${CELLULAR_PREFLIGHT_ONLY:+-DCELLULAR_PREFLIGHT_ONLY=1} ${GIC_V2:+-DGIC_V2=1} ${SUPER_TAIL_START:+-DSUPER_TAIL_START=${SUPER_TAIL_START}ULL} ${SUPER_ROOT_LEN:+-DSUPER_ROOT_LEN=${SUPER_ROOT_LEN}ULL} ${USERDATA_WIN_OFF:+-DUSERDATA_WIN_OFF=${USERDATA_WIN_OFF}ULL -DUSERDATA_WIN_LEN=${USERDATA_WIN_LEN}ULL} -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
# The preflight image must leave the original loader's preferred base free.
LOADER_IMAGE_BASE=0xA1000000
if [ -n "${CELLULAR_PREFLIGHT_ONLY:-}" ]; then LOADER_IMAGE_BASE=0xA3000000; fi
"$LD" /subsystem:efi_application /entry:efi_main /nodefaultlib /base:$LOADER_IMAGE_BASE /out:loader.efi loader.obj
ls -la loader.efi

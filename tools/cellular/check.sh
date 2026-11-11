#!/bin/bash
# Host-only binary-format and actual loader-allocation checks.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 -B -m unittest discover -s tools/cellular/tests -v
CELLULAR_CHECK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cellular-check.XXXXXX")
trap 'rm -rf "$CELLULAR_CHECK_DIR"' EXIT
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/memory_allocator_test.c -o "$CELLULAR_CHECK_DIR/allocator-test"
"$CELLULAR_CHECK_DIR/allocator-test"
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/cellular/tests/spi_pio_test.c -o "$CELLULAR_CHECK_DIR/spi-test"
"$CELLULAR_CHECK_DIR/spi-test"

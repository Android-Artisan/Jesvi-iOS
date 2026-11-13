#!/bin/bash
# Offline release verification and install planning only.
set -euo pipefail
cd "$(dirname "$0")"
exec python3 tools/release/flash_plan.py "$@"

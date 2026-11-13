#!/bin/bash
# Release/developer entry point. This command never flashes a device.
set -euo pipefail
cd "$(dirname "$0")"
exec python3 tools/release/manage.py "$@"

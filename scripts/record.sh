#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Preserve the established X5 VIN bypass workflow. Trailing options override
# defaults, including the memory budget, duration and destination.
exec python3 "$SCRIPT_DIR/../hvs.py" record \
    --x5-vin-bypass \
    --evs-width 768 --evs-height 608 \
    --aps-width 1632 --aps-height 1224 \
    --output /app/recordings/test --seconds 5 \
    --timeout 15 --max-mib 512 "$@"

#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# hvs.py auto selects the existing gbrg color path for preserved Gray8,
# and native display for ISP NV12. Explicit --aps-bayer overrides auto.
exec python3 "$SCRIPT_DIR/../hvs.py" play --output /app/recordings/test "$@"

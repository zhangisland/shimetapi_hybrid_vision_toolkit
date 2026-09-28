#!/usr/bin/env bash
set -euo pipefail

#!/bin/bash
APS_BAYER=none
OUTPUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --aps-bayer)
            APS_BAYER="$2"; shift 2
            ;;
        --output)
            OUTPUT="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--aps-bayer MODE] --output SESSION"
            echo "  --aps-bayer MODE   bayer pattern (default: none)"
            echo "  --output SESSION   session directory to play back (required)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--aps-bayer MODE] --output SESSION" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$OUTPUT" ]]; then
    echo "ERROR: --output SESSION is required" >&2
    echo "Usage: $0 [--aps-bayer MODE] --output SESSION" >&2
    exit 1
fi

python3 hvs.py play --aps-bayer "$APS_BAYER" --output "$OUTPUT"

#!/usr/bin/env bash
set -euo pipefail

#!/bin/bash
PROFILE=1
SECONDS_ARG=5
OUTPUT="/app/recordings/hvs_native_1000"
APS_EXPOSURE_US=1000
APS_WB=off
NO_DISPLAY=1
RECORD=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile)
            PROFILE="$2"; shift 2
            ;;
        --seconds)
            SECONDS_ARG="$2"; shift 2
            ;;
        --output)
            OUTPUT="$2"; shift 2
            ;;
        --aps-exposure-us)
            APS_EXPOSURE_US="$2"; shift 2
            ;;
        --aps-wb)
            APS_WB="$2"; shift 2
            ;;
        --no-display)
            NO_DISPLAY="$2"; shift 2
            ;;
        --record)
            RECORD="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--profile N] [--seconds N] [--output PATH] [--aps-exposure-us US] [--aps-wb MODE] [--no-display 0|1] [--record 0|1]"
            echo "  --profile N              profile id (default: 1)"
            echo "  --seconds N             recording duration in seconds (default: 5)"
            echo "  --output PATH            where to save recording files (default: /app/recordings/hvs_native_1000)"
            echo "  --aps-exposure-us US    APS exposure in microseconds (default: 1000)"
            echo "  --aps-wb MODE           APS white balance mode (default: off)"
            echo "  --no-display 0|1        pass --no-display to hvs.py when 1 (default: 1)"
            echo "  --record 0|1            pass --record to hvs.py when 1 (default: 1)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--profile N] [--seconds N] [--output PATH] [--aps-exposure-us US] [--aps-wb MODE] [--no-display 0|1] [--record 0|1]" >&2
            exit 1
            ;;
    esac
done

ARGS=(--profile "$PROFILE" --aps-exposure-us "$APS_EXPOSURE_US" --aps-wb "$APS_WB")
if [[ "$NO_DISPLAY" == "1" ]]; then ARGS+=(--no-display); fi
if [[ "$RECORD" == "1" ]]; then ARGS+=(--record); fi
ARGS+=(--seconds "$SECONDS_ARG" --output "$OUTPUT")

python3 hvs.py live -- "${ARGS[@]}"

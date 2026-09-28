#!/usr/bin/env bash
set -euo pipefail
#!/bin/bash
PROFILE=1
APS_EXPOSURE_US=1000
APS_GAIN=1
APS_DGAIN=1
SYNC_TOLERANCE_MS=25
SYNC_WAIT_MS=40

while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile)
            PROFILE="$2"; shift 2
            ;;
        --aps-exposure-us)
            APS_EXPOSURE_US="$2"; shift 2
            ;;
        --aps-gain)
            APS_GAIN="$2"; shift 2
            ;;
        --aps-dgain)
            APS_DGAIN="$2"; shift 2
            ;;
        --sync-tolerance-ms)
            SYNC_TOLERANCE_MS="$2"; shift 2
            ;;
        --sync-wait-ms)
            SYNC_WAIT_MS="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--profile N] [--aps-exposure-us US] [--aps-gain G] [--aps-dgain G] [--sync-tolerance-ms MS] [--sync-wait-ms MS]"
            echo "  --profile N              profile id (default: 1)"
            echo "  --aps-exposure-us US     APS exposure in microseconds (default: 5100)"
            echo "  --aps-gain G             APS analog gain (default: 1)"
            echo "  --aps-dgain G            APS digital gain (default: 1)"
            echo "  --sync-tolerance-ms MS   sync tolerance (default: 25)"
            echo "  --sync-wait-ms MS        sync wait (default: 40)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--profile N] [--aps-exposure-us US] [--aps-gain G] [--aps-dgain G] [--sync-tolerance-ms MS] [--sync-wait-ms MS]" >&2
            exit 1
            ;;
    esac
done

python3 hvs.py live -- \
    --profile "$PROFILE" \
    --aps-exposure-us "$APS_EXPOSURE_US" \
    --aps-gain "$APS_GAIN" \
    --aps-dgain "$APS_DGAIN" \
    --sync-tolerance-ms "$SYNC_TOLERANCE_MS" \
    --sync-wait-ms "$SYNC_WAIT_MS"

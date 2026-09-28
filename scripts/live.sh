#!/usr/bin/env bash
set -euo pipefail
# Default params
profile=1
aps_exposure_us=2500
aps_gain=1
aps_dgain=1
sync_tolerance_ms=25
sync_wait_ms=40

# Parse args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) profile="$2"; shift 2;;
    --aps-exposure-us) aps_exposure_us="$2"; shift 2;;
    --aps-gain) aps_gain="$2"; shift 2;;
    --aps-dgain) aps_dgain="$2"; shift 2;;
    --sync-tolerance-ms) sync_tolerance_ms="$2"; shift 2;;
    --sync-wait-ms) sync_wait_ms="$2"; shift 2;;
    *) echo "Unknown option $1"; exit 1;;
  esac
done

python3 hvs.py live -- \
  --profile "$profile" \
  --aps-exposure-us "$aps_exposure_us" \
  --aps-gain "$aps_gain" \
  --aps-dgain "$aps_dgain" \
  --sync-tolerance-ms "$sync_tolerance_ms" \
  --sync-wait-ms "$sync_wait_ms"

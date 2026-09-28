#!/usr/bin/env bash
set -euo pipefail
# Default params
profile=1
seconds=5
output="/app/recordings/hvs_native_2500"
aps_exposure_us=2500
aps_wb=off
no_display=1
record=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) profile="$2"; shift 2;;
    --seconds) seconds="$2"; shift 2;;
    --output) output="$2"; shift 2;;
    --aps-exposure-us) aps_exposure_us="$2"; shift 2;;
    --aps-wb) aps_wb="$2"; shift 2;;
    --no-display) no_display="$2"; shift 2;;
    --record) record="$2"; shift 2;;
    *) echo "Unknown option $1"; exit 1;;
  esac
done

ARGS=(
  --profile "$profile"
  --aps-exposure-us "$aps_exposure_us"
  --aps-wb "$aps_wb"
)
if [[ $no_display -eq 1 ]]; then ARGS+=(--no-display); fi
if [[ $record -eq 1 ]]; then ARGS+=(--record); fi
ARGS+=(--seconds "$seconds" --output "$output")

python3 hvs.py live -- "${ARGS[@]}"

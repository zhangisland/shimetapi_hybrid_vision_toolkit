#!/usr/bin/env bash
set -euo pipefail
# Default params
SESSION=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --session) SESSION="$2"; shift 2;;
    *) echo "Unknown option $1"; exit 1;;
  esac
done

if [[ -z "$SESSION" ]]; then
  echo "ERROR: must supply --session SESSION_PATH"
  exit 1
fi

python3 tools/check_native_recording.py "$SESSION" && \
ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames,r_frame_rate,duration -of json "$SESSION/aps.avi"

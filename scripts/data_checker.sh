#!/usr/bin/env bash
set -euo pipefail

#!/bin/bash
SESSION=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output)
            SESSION="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 --output SESSION"
            echo "  --output SESSION   session directory to check (required)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 --output SESSION" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$SESSION" ]]; then
    echo "ERROR: --output SESSION is required" >&2
    echo "Usage: $0 --output SESSION" >&2
    exit 1
fi

python3 tools/check_native_recording.py "$SESSION" && \
ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames,r_frame_rate,duration -of json "$SESSION/aps.avi"

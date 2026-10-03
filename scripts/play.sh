#!/usr/bin/env bash
set -euo pipefail

#!/bin/bash
WINDOW_WIDTH=1280
WINDOW_HEIGHT=720
OUTPUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --window-width)
            WINDOW_WIDTH="$2"; shift 2
            ;;
        --window-height)
            WINDOW_HEIGHT="$2"; shift 2
            ;;
        --output)
            OUTPUT="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--window-width WINDOW_WIDTH] [--window-height WINDOW_HEIGHT] --output SESSION"
            echo "  --window-width WINDOW_WIDTH player window width (default: 1280)"
            echo " --window-height WINDOW_HEIGHT   player window height (default: 720)"
            echo "  --output SESSION   session directory to play back (required)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--window-width WINDOW_WIDTH] [--window-height WINDOW_HEIGHT] --output SESSION" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$OUTPUT" ]]; then
    echo "ERROR: --output SESSION is required" >&2
    echo "Usage: $0 [--window-width WINDOW_WIDTH] [--window-height WINDOW_HEIGHT] --output SESSION" >&2
    exit 1
fi

python3 hvs.py play --window-width "$WINDOW_WIDTH" --window-height "$WINDOW_HEIGHT" --output "$OUTPUT"

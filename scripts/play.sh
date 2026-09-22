#!/bin/bash

OUTPUT="/app/recordings/test"
SECONDS_ARG=5

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output)
            OUTPUT="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--output PATH] "
            echo "  --output PATH   where to save recording files (default: /app/recordings/test)"
            echo "  --seconds N     recoding duration in seconds (default: 5)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--output PATH] " >&2
            exit 1
            ;;
    esac
done


python3 hvs.py play --output "$OUTPUT" --aps-bayer gbrg

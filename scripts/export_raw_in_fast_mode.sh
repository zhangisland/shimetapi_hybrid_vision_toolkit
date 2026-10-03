#!/usr/bin/env bash
set -euo pipefail

MAX_MIB=1024
INPUT_DIR="/app/recordings/fast_new"
OUTPUT="/app/recordings/fast_new_export"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --max-mib)
            MAX_MIB="$2"; shift 2
            ;;
        --input)
            INPUT_DIR="$2"; shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--max-mib MAX_MIB] --input INPUT_DIR --output OUTPUT_DIR"
            echo "  --max-mib WINDOW_WIDTH player window width (default: 1280)"
            echo "  --input INPUT_DIR  input session directory where aps.vin.zst and evs.vin.zst saved (required)"
            echo "  --output OUTPUT_DIR  output session directory to save exported aps and evs  (required)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--max-mib MAX_MIB] --input INPUT_DIR --output OUTPUT_DIR" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$INPUT_DIR" ]]; then
    echo "ERROR: --input SESSION and --output OUTPUT_DIR are required" >&2
    echo "Usage: $0 [--max-mib MAX_MIB] --input INPUT_DIR --output OUTPUT_DIR" >&2
    exit 1
fi


python3 hvs.py export-recording --max-mib "$MAX_MIB" --input "$INPUT_DIR" --output "$"$OUTPUT_DIR"" 

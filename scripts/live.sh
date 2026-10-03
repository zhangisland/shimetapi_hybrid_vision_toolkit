#!/usr/bin/env bash
set -euo pipefail

I2C_BUS=6
PREVIEW_WIDTH=1280
APS_GAIN_DB=0

while [[ $# -gt 0 ]]; do
    case "$1" in        
        --i2c-bus)
            I2C_BUS="$2"; shift 2
            ;;
        --preview-width)
            PREVIEW_WIDTH="$2"; shift 2
            ;;
        --aps-gain-db)
            APS_GAIN_DB="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--i2c-bus N] [--preview-width PREVIEW_WIDTH] [--aps-gain-db DB]"
            echo "  --i2c-bus N             I2C bus number (default: 6)"
            echo "  --preview-width PREVIEW_WIDTH   preview player width (default: 1280)"
            echo "  --aps-gain-db DB        APS gain in dB (default: 0)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--i2c-bus N] [--preview-width PREVIEW_WIDTH] [--aps-gain-db DB]" >&2
            exit 1
            ;;
    esac
done

ARGS=(--i2c-bus "$I2C_BUS" --preview-width "$PREVIEW_WIDTH" --aps-gain-db "$APS_GAIN_DB")


# python3 hvs.py live --x5-vin-bypass --  --aps-gain-db 0 --i2c-bus 6 --preview-width 960
python3 hvs.py live --x5-vin-bypass -- "${ARGS[@]}"
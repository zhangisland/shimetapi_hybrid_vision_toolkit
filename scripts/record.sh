#!/usr/bin/env bash
set -euo pipefail

#!/bin/bash
X5_VIN_BYPASS=1
STORAGE="memory"
SECONDS_ARG=5
MAX_MIB=1024
I2C_BUS=6
I2C_ADDRESS="0x3c"
APS_GAIN_DB=0
OUTPUT="/app/recordings/gain0_new"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --x5-vin-bypass)
            X5_VIN_BYPASS="$2"; shift 2
            ;;
        --storage)
            STORAGE="$2"; shift 2
            ;;
        --seconds)
            SECONDS_ARG="$2"; shift 2
            ;;
        --max-mib)
            MAX_MIB="$2"; shift 2
            ;;
        --i2c-bus)
            I2C_BUS="$2"; shift 2
            ;;
        --i2c-address)
            I2C_ADDRESS="$2"; shift 2
            ;;
        --aps-gain-db)
            APS_GAIN_DB="$2"; shift 2
            ;;
        --output)
            OUTPUT="$2"; shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--x5-vin-bypass 0|1] [--storage MODE] [--seconds N] [--max-mib N] [--i2c-bus N] [--i2c-address ADDR] [--aps-gain-db DB] [--output PATH]"
            echo "  --x5-vin-bypass 0|1     pass --x5-vin-bypass to hvs.py when 1 (default: 1)"
            echo "  --storage MODE          storage mode (default: memory)"
            echo "  --seconds N             recording duration in seconds (default: 5)"
            echo "  --max-mib N             max recording size in MiB (default: 1024)"
            echo "  --i2c-bus N             I2C bus number (default: 6)"
            echo "  --i2c-address ADDR      I2C address (default: 0x3c)"
            echo "  --aps-gain-db DB        APS gain in dB (default: 0)"
            echo "  --output PATH           where to save recording files (default: /app/recordings/gain0_new)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--x5-vin-bypass 0|1] [--storage MODE] [--seconds N] [--max-mib N] [--i2c-bus N] [--i2c-address ADDR] [--aps-gain-db DB] [--output PATH]" >&2
            exit 1
            ;;
    esac
done

ARGS=(--storage "$STORAGE" --seconds "$SECONDS_ARG" --max-mib "$MAX_MIB" --i2c-bus "$I2C_BUS" --i2c-address "$I2C_ADDRESS" --aps-gain-db "$APS_GAIN_DB" --output "$OUTPUT")
if [[ "$X5_VIN_BYPASS" == "1" ]]; then ARGS+=(--x5-vin-bypass); fi

python3 hvs.py record "${ARGS[@]}"
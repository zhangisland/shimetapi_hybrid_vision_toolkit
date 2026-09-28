#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# events_to_npz.sh
# 用法：只需要传入录像目录名（或完整路径），脚本会自动拼出
#   输入:  <录目录>/events.raw
#   输出:  <录目录>/events.npz
# 然后调用 hvs.py 完成格式转换。
#
# 示例：
#   ./events_to_npz.sh hvs_native_1000
#       -> 转换 /app/recordings/hvs_native_1000/events.raw
#               到 /app/recordings/hvs_native_1000/events.npz
#
#   ./events_to_npz.sh /app/recordings/hvs_native_1000
#       -> 直接使用传入的完整路径
#
#   ./events_to_npz.sh --base /data/recordings my_rec
#       -> 使用自定义根目录
# ============================================================================

# ---- 默认参数 -------------------------------------------------------------
BASE_DIR="/app/recordings"     # 录像根目录；只传目录名时会拼到这里
REC_DIR=""                     # 录像目录（位置参数传入：目录名或完整路径）
HVS_SUBCMD="export-npz"

usage() {
    echo "Usage: $0 [--base DIR] <录像目录名或路径>"
    echo
    echo "  <录像目录名或路径>   必填。可以是目录名（拼到 --base 下）或完整路径"
    echo "  --base DIR           录像根目录 (default: ${BASE_DIR})"
    echo "  -h, --help           显示本帮助"
    echo
    echo "示例:"
    echo "  $0 hvs_native_1000"
    echo "  $0 /app/recordings/hvs_native_1000"
}

# ---- 解析命令行参数 -------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --base)
            BASE_DIR="$2"; shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            # 剩下的第一个位置参数当作录像目录
            if [[ $# -gt 0 ]]; then
                REC_DIR="$1"; shift
            fi
            ;;
        -*)
            echo "Unknown option: $1" >&2
            echo >&2
            usage >&2
            exit 1
            ;;
        *)
            # 位置参数：录像目录（支持传多个时取第一个）
            if [[ -z "$REC_DIR" ]]; then
                REC_DIR="$1"
            fi
            shift
            ;;
    esac
done

# ---- 校验：必须给出录像目录 -----------------------------------------------
if [[ -z "$REC_DIR" ]]; then
    echo "Error: 请指定录像目录（目录名或完整路径）" >&2
    echo >&2
    usage >&2
    exit 1
fi

# ---- 拼出完整路径 ---------------------------------------------------------
# 如果传的不是绝对路径，就拼到 BASE_DIR 下
if [[ "$REC_DIR" != /* ]]; then
    REC_DIR="${BASE_DIR}/${REC_DIR}"
fi

INPUT="${REC_DIR}/events.raw"
OUTPUT="${REC_DIR}/events.npz"

# ---- 前置检查：输入文件必须存在 ------------------------------------------
if [[ ! -d "$REC_DIR" ]]; then
    echo "Error: 录像目录不存在: $REC_DIR" >&2
    exit 1
fi
if [[ ! -f "$INPUT" ]]; then
    echo "Error: 找不到输入文件: $INPUT" >&2
    exit 1
fi

# ---- 执行转换 ------------------------------------------------------------
echo "======================================================"
echo "  Recording dir : $REC_DIR"
echo "  Input  (raw)  : $INPUT"
echo "  Output (npz) : $OUTPUT"
echo "======================================================"

python3 hvs.py "$HVS_SUBCMD" --input "$INPUT" --output "$OUTPUT"

echo "Done -> $OUTPUT"

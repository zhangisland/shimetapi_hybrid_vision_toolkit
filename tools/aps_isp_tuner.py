#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""APS 离线 ISP 调参工具（APX-003CC + RDK X5 VIN bypass 录像）

目的：在不重编 C++ 的前提下，对录制的 Bayer RAW（NV12 AVI 的 Y 平面，
RAW10>>2 后的 8-bit Gray8）迭代 ISP 参数，找到最佳显示效果后，
再把参数固化回 samples/cpp/player/aps_color.h 的 ApsColorEnhance。

用法：
  # 1) 从录像中提取一帧 Bayer 原始数据（Y 平面，W*H 字节）
  python3 aps_isp_tuner.py extract --avi /app/recordings/test/aps.avi \
      --frame 100 --width 1632 --height 1224 --out frame100.raw

  # 2) 用当前参数处理并输出 PNG
  python3 aps_isp_tuner.py process --input frame100.raw --width 1632 \
      --height 1224 --bayer gbrg --config isp_params.json --out out.png

  # 3) 首次使用先导出默认参数，然后编辑 JSON 再跑 process
  python3 aps_isp_tuner.py dump-config --out isp_params.json

  # 4) 自动白平衡：gray-world 或指定白块区域（推荐对白纸/灰卡取块）
  #    在 JSON 里把 "wb_mode" 设为 "gray_world" / "patch" / "manual"。

依赖：numpy、opencv-python（板端 python3 -m pip install opencv-python numpy，
      或在 PC 上跑，把 *.raw / *.avi 拷过来迭代）。

ISP 链（与真实硬件 ISP 顺序一致）：
  黑电平校正 -> 白平衡(gain 作用于 Bayer 马赛克) -> 去马赛克(16bit)
  -> CCM 3x3 -> gamma 编码 -> 8-bit PNG
"""

import argparse
import json
import math
import os
import struct
import subprocess
import sys
from loguru import logger

import numpy as np
import cv2
import time

timestr = time.strftime('%Y%m%d%H%M%S')
logger.add(f'tuning_isp.log')




# OpenCV legacy 枚举映射，与 aps_color.h 保持一致（注意这是反向命名）：
# rggb->BayerBG2BGR, bggr->BayerRG2BGR, grbg->BayerGB2BGR, gbrg->BayerGR2BGR
BAYER_CV_CODE = {
    "rggb": cv2.COLOR_BayerBG2BGR,
    "bggr": cv2.COLOR_BayerRG2BGR,
    "grbg": cv2.COLOR_BayerGB2BGR,
    "gbrg": cv2.COLOR_BayerGR2BGR,
}

# 通道掩码（Bayer 马赛克上按位置取 R/G/B），用于 demosaic 前做逐像素 WB
BAYER_MASK = {
    # (row_parity, col_parity) -> channel; 0=R, 1=G, 2=B
    "rggb": {(0, 0): 0, (0, 1): 1, (1, 0): 1, (1, 1): 2},
    "bggr": {(0, 0): 2, (0, 1): 1, (1, 0): 1, (1, 1): 0},
    "grbg": {(0, 0): 1, (0, 1): 0, (1, 0): 2, (1, 1): 1},
    "gbrg": {(0, 0): 1, (0, 1): 2, (1, 0): 0, (1, 1): 1},
}

DEFAULT_CONFIG = {
    "black_level": 16.0,          # 8-bit 尺度黑电平；RAW10 BL~64 >>2 = 16，标定方法见 README
    "wb_mode": "gray_world",      # gray_world | patch | manual
    "wb_patch": [800, 100, 200, 200],  # wb_mode=patch 时白块区域 x,y,w,h（输入分辨率坐标）
    "wb_gains": [1.0, 1.0, 1.0],  # wb_mode=manual 时 [R, G, B] 增益
    "wb_gain_min": 0.25,
    "wb_gain_max": 4.0,
    "ccm": [[1.0, 0.0, 0.0],
            [0.0, 1.0, 0.0],
            [0.0, 0.0, 1.0]],     # 3x3 色彩校正矩阵（线性域，行和约等于 1）
    "gamma": 2.2,                 # gamma 编码指数，输出 = linear^(1/gamma)
    "saturation": 1.0,            # >1 增饱和，<1 降饱和（在 gamma 之后）
    "brightness": 1.0,            # 输出整体亮度缩放
}


# ---------------------------------------------------------------- AVI 提取

def extract_y_plane_ffmpeg(avi_path: str, frame_idx: int, width: int, height: int, out_path: str) -> bool:
    """用 ffmpeg 提取第 frame_idx 帧的 Y 平面（gray = Y 原值，不做色彩转换）。"""
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-i", avi_path,
        "-vf", "select=eq(n\\,%d)" % frame_idx,
        "-frames:v", "1", "-pix_fmt", "gray",
        "-f", "rawvideo", out_path,
    ]
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        return False
    if res.returncode != 0 or not os.path.exists(out_path):
        return False
    return os.path.getsize(out_path) == width * height


def list_avi_frames(avi_path: str):
    """纯 Python 解析 AVI 的 'movi' 列表，返回 [(fourcc, data_offset, size)]。"""
    with open(avi_path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise ValueError("不是 AVI 文件: %s" % avi_path)
    # 找到 movi LIST
    pos, movi_start = 12, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"LIST":
            ltype = data[pos + 8:pos + 12]
            if ltype == b"movi":
                # LIST 布局: "LIST" <size:4> "movi" <data...>，数据从 pos+12 开始
                movi_start = pos + 12
                break
        pos += 8 + size + (size & 1)
    if movi_start is None:
        raise ValueError("AVI 中未找到 movi 列表")
    frames, pos = [], movi_start
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if not (0x30 <= cid[0] <= 0x39):  # 非 ##dc/##db 帧块则继续
            if cid in (b"JUNK", b"idx1"):
                break
            pos += 8 + size + (size & 1)
            continue
        frames.append((cid, pos + 8, size))
        pos += 8 + size + (size & 1)
    return data, frames


def extract_y_plane_python(avi_path: str, frame_idx: int, width: int, height: int, out_path: str) -> None:
    """无 ffmpeg 时的回退：假设是无压缩 NV12 AVI，取帧数据前 W*H 字节为 Y 平面。"""
    data, frames = list_avi_frames(avi_path)
    if frame_idx >= len(frames):
        raise ValueError("帧号 %d 超出范围（共 %d 帧）" % (frame_idx, len(frames)))
    cid, off, size = frames[frame_idx]
    need = width * height * 3 // 2
    if size < need:
        raise ValueError("帧数据 %d 字节小于 NV12 帧大小 %d（fourcc=%s）"
                         % (size, need, cid.decode("latin1")))
    with open(out_path, "wb") as f:
        f.write(data[off:off + width * height])
    logger.info("提取到 Y 平面 %d 字节（帧块 %s, %d 字节，共 %d 帧）"
          % (width * height, cid.decode("latin1"), size, len(frames)))


# ---------------------------------------------------------------- ISP 链

def bayer_channel_map(pattern: str, width: int, height: int) -> np.ndarray:
    """生成与 Bayer 排列对应的通道索引图（0=R,1=G,2=B）。"""
    m = BAYER_MASK[pattern]
    idx = np.zeros((height, width), dtype=np.uint8)
    for (r, c), ch in m.items():
        idx[r::2, c::2] = ch
    return idx


def compute_wb_gains(raw01: np.ndarray, cfg: dict, pattern: str) -> np.ndarray:
    """返回 [R, G, B] 白平衡增益（以 G 为基准）。"""
    ch_map = bayer_channel_map(pattern, raw01.shape[1], raw01.shape[0])
    lo, hi = float(cfg["wb_gain_min"]), float(cfg["wb_gain_max"])

    def clamp(g):
        return np.clip(g, lo, hi)

    mode = cfg["wb_mode"]
    if mode == "gray_world":
        means = [raw01[ch_map == c].mean() for c in range(3)]
        g = clamp(np.array([means[1] / m if m > 1e-6 else 1.0 for m in means]))
        return g / g[1]  # 保持 G 增益为 1
    if mode == "patch":
        x, y, w, h = cfg["wb_patch"]
        patch = raw01[y:y + h, x:x + w]
        pmap = ch_map[y:y + h, x:x + w]
        means = [patch[pmap == c].mean() for c in range(3)]
        g = clamp(np.array([means[1] / m if m > 1e-6 else 1.0 for m in means]))
        return g / g[1]
    if mode == "manual":
        return np.clip(np.array(cfg["wb_gains"], dtype=np.float64), lo, hi)
    raise ValueError("未知 wb_mode: %s" % mode)


def apply_ccm(img_lin: np.ndarray, ccm) -> np.ndarray:
    m = np.array(ccm, dtype=np.float64)
    if m.shape != (3, 3):
        raise ValueError("ccm 必须是 3x3")
    r, g, b = img_lin[..., 2], img_lin[..., 1], img_lin[..., 0]  # BGR 顺序
    # CCM 按 RGB 顺序定义
    nr = m[0, 0] * r + m[0, 1] * g + m[0, 2] * b
    ng = m[1, 0] * r + m[1, 1] * g + m[1, 2] * b
    nb = m[2, 0] * r + m[2, 1] * g + m[2, 2] * b
    out = np.stack([nb, ng, nr], axis=-1)
    return np.clip(out, 0.0, 1.0)


def adjust_saturation(img: np.ndarray, s: float) -> np.ndarray:
    if abs(s - 1.0) < 1e-3:
        return img
    hsv = cv2.cvtColor((img * 255).astype(np.uint8), cv2.COLOR_BGR2HSV).astype(np.float32)
    hsv[..., 1] = np.clip(hsv[..., 1] * s, 0, 255)
    return cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR).astype(np.float64) / 255.0


def run_isp(raw_u8: np.ndarray, cfg: dict, pattern: str) -> np.ndarray:
    h, w = raw_u8.shape
    raw01 = raw_u8.astype(np.float64) / 255.0

    # 1) 黑电平校正 + 归一化
    bl = float(cfg["black_level"]) / 255.0
    if bl > 0:
        raw01 = np.clip((raw01 - bl) / (1.0 - bl), 0.0, 1.0)

    # 2) 白平衡（作用于 Bayer 马赛克，逐像素按通道位置乘增益）
    gains = compute_wb_gains(raw01, cfg, pattern)
    logger.info("WB gains [R, G, B] = %s" % np.round(gains, 4).tolist())
    ch_map = bayer_channel_map(pattern, w, h)
    wb = gains[ch_map]
    raw01 = np.clip(raw01 * wb, 0.0, 1.0)

    # 3) 去马赛克（16-bit 保精度）
    raw16 = (raw01 * 65535.0 + 0.5).astype(np.uint16)
    bgr16 = cv2.demosaicing(raw16, BAYER_CV_CODE[pattern])
    lin = bgr16.astype(np.float64) / 65535.0

    # 4) CCM（线性域）
    lin = apply_ccm(lin, cfg["ccm"])

    # 5) gamma 编码 + 亮度
    gamma = float(cfg["gamma"])
    if gamma > 0 and abs(gamma - 1.0) > 1e-3:
        lin = np.power(lin, 1.0 / gamma)
    lin = lin * float(cfg["brightness"])

    # 6) 饱和度
    out8 = (np.clip(lin, 0, 1) * 255 + 0.5).astype(np.uint8)
    out8 = adjust_saturation(out8.astype(np.float64) / 255.0, float(cfg["saturation"]))
    return (np.clip(out8, 0, 1) * 255 + 0.5).astype(np.uint8)


def save_image(path: str, img: np.ndarray) -> None:
    """cv2.imwrite 在 Windows 上不支持含中文/非 ASCII 的路径且静默失败，
    统一改用 imencode + Python 原生文件写入，并显式检查结果。"""
    ext = os.path.splitext(path)[1] or ".png"
    ok, buf = cv2.imencode(ext, img)
    if not ok:
        raise RuntimeError("图像编码失败: %s" % path)
    with open(path, "wb") as f:
        f.write(buf.tobytes())


# ---------------------------------------------------------------- 命令

def cmd_extract(a):
    ok = extract_y_plane_ffmpeg(a.avi, a.frame, a.width, a.height, a.out)
    if ok:
        logger.info("ffmpeg 提取成功: %s (%d 字节)" % (a.out, a.width * a.height))
        return
    logger.info("ffmpeg 不可用或失败，改用纯 Python AVI 解析…")
    extract_y_plane_python(a.avi, a.frame, a.width, a.height, a.out)


def cmd_process(a):
    cfg = dict(DEFAULT_CONFIG)
    if a.config and os.path.exists(a.config):
        with open(a.config, "r", encoding="utf-8") as f:
            cfg.update(json.load(f))
    raw = np.fromfile(a.input, dtype=np.uint8)
    if raw.size != a.width * a.height:
        raise SystemExit("数据大小 %d 与 W*H=%d 不符（应为 Gray8 Y 平面）"
                         % (raw.size, a.width * a.height))
    raw = raw.reshape(a.height, a.width)
    out = run_isp(raw, cfg, a.bayer)
    save_image(a.out, out)
    logger.info("已输出: %s  (参数: BL=%s, wb_mode=%s, gamma=%s, sat=%s)"
          % (a.out, cfg["black_level"], cfg["wb_mode"], cfg["gamma"], cfg["saturation"]))
    # 同时输出不做任何 ISP 的线性 demosaic 参照图
    if a.with_baseline:
        base = cv2.demosaicing(raw, BAYER_CV_CODE[a.bayer])
        save_image(os.path.splitext(a.out)[0] + "_baseline.png", base)
        logger.info("已输出参照图: %s_baseline.png" % os.path.splitext(a.out)[0])


def cmd_stats(a):
    """打印 Bayer RAW 的逐通道统计，用于诊断偏色来源。
    --dark 模式：输入为盖镜头盖的黑帧，直接给出建议 black_level。"""
    raw = np.fromfile(a.input, dtype=np.uint8)
    if raw.size != a.width * a.height:
        raise SystemExit("数据大小 %d 与 W*H=%d 不符" % (raw.size, a.width * a.height))
    raw = raw.reshape(a.height, a.width)
    ch_map = bayer_channel_map(a.bayer, a.width, a.height)
    if a.dark:
        bl = float(np.median(raw))
        logger.info("黑帧中位数（建议 black_level）= %.1f (8-bit)  [RAW10 尺度约 %.0f]"
              % (bl, bl * 4))
        logger.info("黑帧各通道均值 R=%.1f G=%.1f B=%.1f（应接近 black_level，偏差大说明有通道性底噪）"
              % tuple(raw[ch_map == c].mean() for c in range(3)))
        return
    logger.info("通道 |  min   p1   中位数  均值   p99   max  | 饱和率(>=250)")
    for c, name in enumerate("RGB"):
        v = raw[ch_map == c].astype(np.float64)
        p = np.percentile(v, [1, 50, 99])
        logger.info("  %s  | %4d %5.0f %6.0f %6.1f %5.0f %5d  |   %.2f%%"
              % (name, v.min(), p[0], p[1], v.mean(), p[2], int(v.max()),
                 100.0 * (v >= 250).mean()))
    dark_med = float(np.median(raw[raw <= np.percentile(raw, 1)]))
    logger.info("最暗 1%% 像素中位数 ≈ %.1f（若明显大于配置的 black_level，请修正 black_level）" % dark_med)


def cmd_dump_config(a):
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(DEFAULT_CONFIG, f, indent=2, ensure_ascii=False)
    logger.info("默认参数已写入 %s，编辑后用 --config 传入" % a.out)


def main():
    logger.info(f'.....start....')
    logger.info(f'python {" ".join(sys.argv)}')
    p = argparse.ArgumentParser(description="APS 离线 ISP 调参工具")
    sub = p.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("extract", help="从录像 AVI 提取一帧 Bayer(Y平面) 原始数据")
    e.add_argument("--avi", required=True)
    e.add_argument("--frame", type=int, default=0)
    e.add_argument("--width", type=int, required=True)
    e.add_argument("--height", type=int, required=True)
    e.add_argument("--out", required=True)
    e.set_defaults(func=cmd_extract)

    pr = sub.add_parser("process", help="对提取的 .raw 执行 ISP 链并输出 PNG")
    pr.add_argument("--input", required=True)
    pr.add_argument("--width", type=int, required=True)
    pr.add_argument("--height", type=int, required=True)
    pr.add_argument("--bayer", default="gbrg", choices=list(BAYER_CV_CODE))
    pr.add_argument("--config", default="isp_params.json")
    pr.add_argument("--out", default="isp_out.png")
    pr.add_argument("--with-baseline", action="store_true", help="同时输出无 ISP 参照图")
    pr.set_defaults(func=cmd_process)

    d = sub.add_parser("dump-config", help="导出默认参数 JSON")
    d.add_argument("--out", default="isp_params.json")
    d.set_defaults(func=cmd_dump_config)

    s = sub.add_parser("stats", help="打印 Bayer RAW 逐通道统计（诊断偏色/黑电平/饱和）")
    s.add_argument("--input", required=True)
    s.add_argument("--width", type=int, required=True)
    s.add_argument("--height", type=int, required=True)
    s.add_argument("--bayer", default="gbrg", choices=list(BAYER_CV_CODE))
    s.add_argument("--dark", action="store_true",
                   help="输入为盖镜头盖黑帧：输出建议 black_level")
    s.set_defaults(func=cmd_stats)

    a = p.parse_args()
    logger.info(a)
    a.func(a)
    logger.info(f'.....end....')



if __name__ == "__main__":
    main()

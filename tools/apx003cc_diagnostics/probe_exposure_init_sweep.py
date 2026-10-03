#!/usr/bin/env python3
"""probe_exposure_init_sweep.py — init-time override sweep to locate the APS
integration-time register.

Verdict so far (see APX003CC_EXPOSURE_INVESTIGATION_LOG.md):
  - Runtime writes to the 0x3Axx timing block are flat (probe_exposure_3a05.py).
  - Init-time override of 0x3A05/06 (1500 -> 750 -> 200) via APX_OVERRIDE is ALSO
    flat (raw_mean pinned at ~77). So 0x3A05/06 is NOT the integration time.

This script sweeps the remaining plausible 16-bit "lines-like" registers in the
vendor init table. For each candidate it re-initialises the sensor with the value
HALVED (via the LD_PRELOAD APX_OVERRIDE interposer) and measures raw_mean.

Built-in controls make the result self-validating:
  - POSITIVE control 0x3253 (frame-length / fps divider): override 0x28 -> 0x30
    (30 -> 36 fps) shortens per-frame integration => raw_mean should DROP. If it
    moves, the override pipeline is proven working and any flat candidate is a
    trustworthy negative.
  - NEGATIVE control 0x3A05/06 (already ruled out): should stay flat.

Run on the board desktop session, bright APS scene, after compiling the interposer:
    gcc -shared -fPIC -O2 -o tools/apx003cc_diagnostics/passive_i2c_trace.so \
        tools/apx003cc_diagnostics/passive_i2c_trace.c -ldl -lpthread
    python3 tools/apx003cc_diagnostics/probe_exposure_init_sweep.py

Interpretation: a candidate whose raw_mean scales ~linearly with the override is
the integration time (init-latched; set it once before streaming, no runtime AE).
"""
import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time
from collections import deque

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SO_PATH = os.path.join(ROOT, "tools", "apx003cc_diagnostics", "passive_i2c_trace.so")
TRACE_LOG = "/tmp/sweep_trace.log"

# (label, kind, hi_reg, lo_reg_or_value) — 16-bit big-endian (lower addr = high byte)
# for 16-bit candidates; kind="byte" for single-byte controls (lo field is the byte).
CANDIDATES = [
    # --- controls ---
    ("0x3253  [POSITIVE: fps divider, expect DARKER]", "byte", 0x3253, 0x30),
    ("0x3A05/06 [NEGATIVE: ruled out, expect flat]", "u16", 0x3A05, 0x3A06),
    # --- 0x3Axx block, lines-like values ---
    ("0x3A0E/0F", "u16", 0x3A0E, 0x3A0F),
    ("0x3A12/13", "u16", 0x3A12, 0x3A13),
    ("0x3A26/27", "u16", 0x3A26, 0x3A27),
    ("0x3A07/08", "u16", 0x3A07, 0x3A08),
    # --- 0x35xx block, lines-like values ---
    ("0x3503/04", "u16", 0x3503, 0x3504),
    ("0x3505/06", "u16", 0x3505, 0x3506),
    ("0x3512/13", "u16", 0x3512, 0x3513),
    ("0x3514/15", "u16", 0x3514, 0x3515),
    ("0x3522/23", "u16", 0x3522, 0x3523),
]

# vendor init values (ground truth from the i2c-dev trace), keyed by hi_reg.
ORIG16 = {
    0x3A05: 0x05DC,  # 1500 (negative control)
    0x3A0E: 0x0960,  # 2400
    0x3A12: 0x0064,  # 100
    0x3A26: 0x00C8,  # 200
    0x3A07: 0x0050,  # 80
    0x3503: 0x0410,  # 1040
    0x3505: 0x0400,  # 1024
    0x3512: 0x0715,  # 1813
    0x3514: 0x0A26,  # 2598
    0x3522: 0x0100,  # 256
}
BYTE_ORIG = {0x3253: 0x28}   # 40 -> 30 fps


class Preview:
    def __init__(self, width):
        self.width = width
        self.proc = None
        self.samples = deque(maxlen=64)
        self.last_seq = -1

    def start(self, override):
        cmd = [sys.executable, "hvs.py", "live", "--x5-vin-bypass", "--",
               "--preview-width", str(self.width), "--no-verify"]
        env = os.environ.copy()
        env["LD_PRELOAD"] = SO_PATH
        env["I2C_TRACE_LOG"] = TRACE_LOG
        if override:
            env["APX_OVERRIDE"] = override
        else:
            env.pop("APX_OVERRIDE", None)
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     bufsize=1, env=env)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            sys.stderr.write("[preview] " + line)
            sys.stderr.flush()
            m = re.search(r"raw_mean=([0-9.]+).*raw_sample_sequence=(\d+)", line)
            if m:
                seq = int(m.group(2))
                if seq > self.last_seq:          # only fresh frames
                    self.last_seq = seq
                    self.samples.append((time.monotonic(), float(m.group(1))))

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def measure(self, settle, window):
        self.samples.clear()
        self.last_seq = -1
        # Phase 1: wait for the FIRST fresh frame. Sensor init (search + dual VIN
        # open) takes several seconds; a fixed settle window counted from process
        # start closes before the first frame arrives and yields None.
        first_deadline = time.monotonic() + 30.0
        while time.monotonic() < first_deadline:
            if not self.alive():
                return None
            if self.samples:
                break
            time.sleep(0.1)
        if not self.samples:
            return None
        # Phase 2: settle, then average the last `window` seconds of samples.
        deadline = time.monotonic() + settle + window
        while time.monotonic() < deadline:
            if not self.alive():
                return None
            time.sleep(0.1)
        recent = [v for t, v in self.samples if t >= deadline - window]
        return sum(recent) / len(recent) if recent else None

    def stop(self):
        if self.alive():
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.terminate()


def override_str(hi_reg, val16):
    """16-bit big-endian: lower address holds the HIGH byte (matches 0x3A05/06)."""
    return "0x%04X=0x%02X,0x%04X=0x%02X" % (
        hi_reg, (val16 >> 8) & 0xFF, hi_reg + 1, val16 & 0xFF)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--preview-width", type=int, default=960)
    ap.add_argument("--settle", type=float, default=1.5)
    ap.add_argument("--window", type=float, default=1.5)
    ap.add_argument("--skip", type=int, default=0,
                    help="skip the first N candidates (resume)")
    args = ap.parse_args()

    if not os.path.exists(SO_PATH):
        raise RuntimeError(f"interposer not found: {SO_PATH}\n"
                           f"build it first:\n  gcc -shared -fPIC -O2 -o "
                           f"{SO_PATH} {SO_PATH[:-3]}.c -ldl -lpthread")
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("preview needs a display; run from the board desktop session")

    preview = Preview(args.preview_width)
    results = []

    def run(label, override):
        preview.start(override)
        v = preview.measure(args.settle, args.window)
        preview.stop()
        return v

    try:
        print(f"== probe_exposure_init_sweep ==\n"
              f"so={SO_PATH} trace={TRACE_LOG}", flush=True)
        base = run("baseline (no override)", None)
        print(f"baseline raw_mean = {base:.2f}\n", flush=True)

        for i, (label, kind, hi, lo) in enumerate(CANDIDATES):
            if i < args.skip:
                continue
            if kind == "byte":
                ov = "0x%04X=0x%02X" % (hi, lo)
                orig = BYTE_ORIG.get(hi, 0)
            else:
                orig = ORIG16.get(hi)
                if orig is None:
                    print(f"[skip] {label}: no vendor value recorded", flush=True)
                    continue
                ov = override_str(hi, orig // 2)
            mean = run(label, ov)
            if mean is None:
                print(f"{label}: PREVIEW DIED (override {ov})", flush=True)
                results.append((label, None, ov))
                continue
            ratio = mean / base * 100.0 if base else 0.0
            delta = mean - base
            print(f"{label}: override {ov} -> raw_mean={mean:.2f} "
                  f"(delta={delta:+.2f}, {ratio:.1f}%)", flush=True)
            results.append((label, mean, ov))

        print("\n== summary ==", flush=True)
        for label, mean, ov in results:
            if mean is None:
                print(f"  DIED      {label}", flush=True)
            else:
                delta = mean - base
                print(f"  {'MOVED' if abs(delta) > base * 0.08 else 'flat  '}  "
                      f"delta={delta:+7.2f}  {label}", flush=True)
        print("\nRule: a candidate with |delta| > ~8% of baseline is the integration "
              "time (halving it darkens). Positive control 0x3253 must show MOVED, "
              "otherwise the override pipeline is broken and flat results are void.",
              flush=True)
    finally:
        preview.stop()


if __name__ == "__main__":
    sys.exit(main())

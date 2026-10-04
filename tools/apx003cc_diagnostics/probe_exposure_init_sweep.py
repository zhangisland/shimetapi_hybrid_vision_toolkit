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

# (label, kind, hi_reg, ov_value): kind="byte" -> single-byte override of hi_reg;
# kind="u16" -> 16-bit big-endian override (hi_reg = high byte, hi_reg+1 = low).
# ov_value is the value to WRITE (reduced from the vendor init value).
CANDIDATES = [
    # POSITIVE brightness control: gain reg 0x3602 vendor init 0x78 (=0dB).
    # 0x10 = 24dB code (verified anchor in v2: raw_mean 77 -> ~564). Preview is
    # started without --aps-gain-db so the C++ side never touches I2C and the
    # init override is NOT overwritten at runtime. If this does not brighten,
    # the sensor response side is broken and every flat below is void.
    ("0x3602  [POSITIVE: gain 0dB->24dB, expect BIG brighten]", "byte", 0x3602, 0x10),
    # NEGATIVE control (already ruled out, must stay flat).
    ("0x3A05/06 [NEGATIVE: ruled out, expect flat]", "u16", 0x3A05, 0x02EE),  # 1500->750
    # --- died at 50% override: gentle retest ---
    ("0x3503/04 gentle 1040->920 (-12%)", "u16", 0x3503, 0x0398),
    ("0x3514/15 gentle 2598->2400 (-7.6%) [VTS? 2598*12.8us=30fps]", "u16", 0x3514, 0x0960),
    # --- Sony coarse-integration style block, NEVER init-tested before ---
    ("0x3500/01 coarse 0x0100(256)->0x0080(128)", "u16", 0x3500, 0x0080),
    ("0x3502 fine 0x03->0x01", "byte", 0x3502, 0x01),
]


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
        """Wait for the first fresh frame, then average `window` seconds of
        raw_mean. Also derives fps from the sample timestamps. Returns
        (mean, fps) or None if the preview died / produced no fresh frames."""
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
        pts = [(t, v) for t, v in self.samples if t >= deadline - window]
        if not pts:
            return None
        mean = sum(v for _, v in pts) / len(pts)
        fps = (len(pts) - 1) / (pts[-1][0] - pts[0][0]) if len(pts) >= 2 else 0.0
        return mean, fps

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
        if base is None:
            raise RuntimeError("baseline measurement failed: preview produced "
                               "no fresh frames (check [preview] output above)")
        bmean, bfps = base
        print(f"baseline raw_mean = {bmean:.2f}  fps~{bfps:.1f}\n", flush=True)

        for i, (label, kind, hi, val) in enumerate(CANDIDATES):
            if i < args.skip:
                continue
            if kind == "byte":
                ov = "0x%04X=0x%02X" % (hi, val)
            else:
                ov = override_str(hi, val)
            res = run(label, ov)
            if res is None:
                print(f"{label}: PREVIEW DIED (override {ov})", flush=True)
                results.append((label, None, None, ov))
                continue
            mean, fps = res
            ratio = mean / bmean * 100.0
            delta = mean - bmean
            print(f"{label}: override {ov} -> raw_mean={mean:.2f} "
                  f"(delta={delta:+.2f}, {ratio:.1f}%) fps~{fps:.1f}", flush=True)
            results.append((label, mean, fps, ov))

        print("\n== summary ==", flush=True)
        for label, mean, fps, ov in results:
            if mean is None:
                print(f"  DIED      {label}", flush=True)
            else:
                delta = mean - bmean
                tag = "MOVED" if abs(delta) > bmean * 0.08 else "flat  "
                print(f"  {tag}  delta={delta:+7.2f}  fps~{fps:5.1f}  {label}",
                      flush=True)
        print("\nRules:\n"
              "  1. POSITIVE control 0x3602 (gain 0->24dB) MUST show MOVED with a "
              "big brighten; otherwise the sensor response side is broken and all "
              "flat results are void.\n"
              "  2. NEGATIVE control 0x3A05/06 must stay flat.\n"
              "  3. Any other candidate with |delta| > 8% of baseline is the "
              "integration time -> wire it into vin_record.cpp before stream-on.\n"
              "  4. If 0x3514/15 shows fps UP but brightness flat, it is VTS "
              "(frame length) and exposure lines are fixed elsewhere.",
              flush=True)
    finally:
        preview.stop()


if __name__ == "__main__":
    sys.exit(main())

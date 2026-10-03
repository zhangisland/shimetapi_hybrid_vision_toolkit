#!/usr/bin/env python3
"""APX003CC bright-field APS exposure probe: static candidates + 0x3603 index-port scan.

Run ON the RDK X5 board, in the toolkit root, from a desktop session, with a
BRIGHT scene in front of the APS lens (outdoor / strong lamp):

    python3 tools/apx003cc_diagnostics/probe_aps_exposure_v2.py

Optional: --i2c-bus 6 --settle 1.5 --window 2.0 --index-start 0 --index-end 63

Method (see APX003CC_PRIOR_KNOWLEDGE.md sections 4/5/6/7):
  1. Self-start preview (hv_hvs_record_vin --preview --no-verify, no gain arg,
     so the process never touches I2C) and wait for raw_mean telemetry.
  2. Anchor check: 24 dB through the verified gain chain must move raw_mean by
     more than 30 percent, otherwise the scene is not signal-dominated and any
     "no response" would be meaningless -> abort without conclusions.
  3. Part A static candidates: 0x3A05/06 (factory 0x05DC=1500), 0x3600/01
     (0x0400=1024), 0x3B03/04 (0x018C=396), 0x3500/01/02. Save originals,
     write low, measure, write high, measure, restore, read back retention.
  4. Part B index port: 0x3603=0..63 (skip 7 = gain), each with the full
     gain-style handshake {0x3660=1, 0x3602=val, 0x3661=1, 0x3662=0, latch}.
     A real exposure sub-field shows a sustained >10 percent low-vs-high
     raw_mean shift. Drift reference is re-measured every 8 indexes.
  5. Final report sorted by optical delta; full log saved next to this script.
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
GAIN_INDEX = 7
LATCH = 0x342C

STATIC_CANDIDATES = [
    ("0x3A05/3A06 lines-like", [0x3A05, 0x3A06], [0x00, 0x01], [0x0A, 0x00]),
    ("0x3600/3601", [0x3600, 0x3601], [0x00, 0x01], [0x0A, 0x00]),
    ("0x3B03/3B04", [0x3B03, 0x3B04], [0x00, 0x01], [0x0A, 0x00]),
    ("0x3500/3501/3502", [0x3500, 0x3501, 0x3502], [0x00, 0x00, 0x00], [0x0F, 0xFF, 0xFF]),
]


class Probe:
    def __init__(self, bus, addr_hex):
        self.bus, self.addr = bus, addr_hex
        self.log_lines = []

    def log(self, text):
        print(text, flush=True)
        self.log_lines.append(text)

    # ---- raw I2C (combined-transaction read is mandatory, two-step reads 0) ----
    def write_reg(self, reg, val):
        cmd = ["i2ctransfer", "-y", "-f", str(self.bus),
               "w3@" + self.addr, f"0x{reg >> 8:02x}", f"0x{reg & 0xFF:02x}", f"0x{val:02x}"]
        return subprocess.run(cmd, capture_output=True, text=True).returncode == 0

    def read_reg(self, reg):
        cmd = ["i2ctransfer", "-y", "-f", str(self.bus),
               "w2@" + self.addr, f"0x{reg >> 8:02x}", f"0x{reg & 0xFF:02x}", "r1"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            return None
        match = re.search(r"0x([0-9a-fA-F]{2})", result.stdout)
        return int(match.group(1), 16) if match else None

    def write_regs(self, pairs):
        ok = all(self.write_reg(r, v) for r, v in pairs)
        retention = [(r, v, self.read_reg(r)) for r, v in pairs]
        lost = [f"0x{r:04X}(want 0x{v:02X} got {('0x%02X' % a) if a is not None else 'NACK'})"
                for r, v, a in retention if a != v]
        return ok, lost

    def latch(self):
        for value in (0, 1, 0):
            self.write_reg(LATCH, value)

    def field_write(self, index, value):
        """Full gain-style handshake through the 0x3603 selector port."""
        self.write_reg(0x3603, index)
        self.write_reg(0x3660, 1)
        self.write_reg(0x3602, value)
        self.write_reg(0x3661, 1)
        self.write_reg(0x3662, 0)
        self.latch()


class Preview:
    """Owns the preview process and parses raw_mean telemetry from its stdout."""

    def __init__(self, width):
        self.width = width
        self.proc = None
        self.samples = deque(maxlen=64)
        self.restarts = 0

    def start(self):
        cmd = [sys.executable, "hvs.py", "live", "--x5-vin-bypass", "--",
               "--preview-width", str(self.width), "--no-verify"]
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            match = re.search(r"raw_mean=([0-9.]+)", line)
            if match:
                self.samples.append((time.monotonic(), float(match.group(1))))
            elif line.startswith(("Preview APS", "+ ")):
                continue  # per-second telemetry: parsed silently
            else:
                print("  [preview] " + line.rstrip(), flush=True)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def ensure(self):
        if not self.alive():
            if self.restarts >= 3:
                raise RuntimeError("preview died three times; aborting probe")
            self.restarts += 1
            self.log(f"  preview not running (restart {self.restarts}/3); restarting")
            self.start()

    def measure(self, settle, window):
        self.samples.clear()
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


def measure_or_restart(preview, probe, settle, window, what):
    for _ in range(2):
        preview.ensure()
        value = preview.measure(settle, window)
        if value is not None:
            return value
        probe.log(f"  measurement lost during {what}; preview restart")
    raise RuntimeError(f"cannot measure {what}; aborting")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--i2c-bus", type=int, default=6)
    parser.add_argument("--i2c-address", default="0x3c")
    parser.add_argument("--preview-width", type=int, default=640)
    parser.add_argument("--settle", type=float, default=1.5)
    parser.add_argument("--window", type=float, default=2.0)
    parser.add_argument("--index-start", type=int, default=0)
    parser.add_argument("--index-end", type=int, default=63)
    args = parser.parse_args()

    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("preview needs a display; run from the board desktop session")
    probe = Probe(args.i2c_bus, args.i2c_address)
    preview = Preview(args.preview_width)
    hits = []
    try:
        probe.log(f"== probe_aps_exposure_v2 bus={args.i2c_bus} addr={args.i2c_address} ==\n")
        probe.log("step 1: starting preview (no gain arg -> process never touches I2C)")
        preview.start()
        baseline = measure_or_restart(preview, probe, 4.0, args.window, "initial baseline")
        probe.log(f"baseline raw_mean={baseline:.2f}\n")

        probe.log("step 2: anchor check, 24 dB via verified gain chain (index 7)")
        probe.field_write(GAIN_INDEX, 0x10)
        anchor = measure_or_restart(preview, probe, args.settle, args.window, "anchor")
        moved = (anchor - baseline) / baseline * 100.0
        probe.log(f"anchor raw_mean={anchor:.2f} ({moved:+.1f} percent vs baseline)")
        if moved < 30.0:
            probe.log("ABORT: scene not bright enough / telemetry insensitive; "
                      "aim the APS lens at a bright scene and rerun (PRIOR_KNOWLEDGE section 6)")
            return 2
        probe.field_write(GAIN_INDEX, 0xFF)
        recovered = measure_or_restart(preview, probe, args.settle, args.window, "gain recovery")
        probe.log(f"gain restored to 0 dB, raw_mean={recovered:.2f}\n")
        baseline = recovered

        probe.log("step 3 (Part A): static candidate registers, low vs high, then restore")
        for name, regs, low, high in STATIC_CANDIDATES:
            originals = [(r, probe.read_reg(r)) for r in regs]
            if any(v is None for _, v in originals):
                probe.log(f"{name}: read failed (sensor NACK?), skipped")
                continue
            probe.log(f"{name}: original " + " ".join(f"0x{r:04X}=0x{v:02X}" for r, v in originals))
            ok_low, lost_low = probe.write_regs(list(zip(regs, low)))
            mean_low = measure_or_restart(preview, probe, args.settle, args.window, f"{name} low")
            ok_high, lost_high = probe.write_regs(list(zip(regs, high)))
            mean_high = measure_or_restart(preview, probe, args.settle, args.window, f"{name} high")
            probe.write_regs(originals)
            mean_back = measure_or_restart(preview, probe, args.settle, args.window, f"{name} restore")
            delta = (mean_high - mean_low) / max(mean_high, mean_low) * 100.0 if mean_low and mean_high else 0.0
            verdict = "HIT" if abs(delta) > 10.0 else "flat"
            probe.log(f"{name}: low={mean_low:.2f} high={mean_high:.2f} delta={delta:+.1f} percent -> {verdict}"
                      f" | restore mean={mean_back:.2f}"
                      f" | retention: low {'OK' if ok_low and not lost_low else 'LOST ' + ','.join(lost_low)}"
                      f", high {'OK' if ok_high and not lost_high else 'LOST ' + ','.join(lost_high)}")
            if abs(delta) > 10.0:
                hits.append((name, delta, mean_low, mean_high))
            baseline = mean_back
        probe.log("")

        probe.log(f"step 4 (Part B): index port 0x3603 scan {args.index_start}..{args.index_end} (skip {GAIN_INDEX})")
        for index in range(args.index_start, args.index_end + 1):
            if index == GAIN_INDEX:
                continue
            if index % 8 == args.index_start % 8:
                probe.field_write(GAIN_INDEX, 0xFF)
                drift = measure_or_restart(preview, probe, args.settle, args.window, "drift reference")
                probe.log(f"  drift reference (index 7, 0 dB): raw_mean={drift:.2f}")
            probe.field_write(index, 0x00)
            mean_low = measure_or_restart(preview, probe, args.settle, args.window, f"index {index} low")
            probe.field_write(index, 0xFF)
            mean_high = measure_or_restart(preview, probe, args.settle, args.window, f"index {index} high")
            kept_low = probe.read_reg(0x3603)
            delta = (mean_high - mean_low) / max(mean_high, mean_low) * 100.0 if mean_low and mean_high else 0.0
            verdict = "HIT" if abs(delta) > 10.0 else "flat"
            probe.log(f"index {index:2d}: low={mean_low:.2f} high={mean_high:.2f} delta={delta:+.1f} percent -> {verdict}"
                      + ("" if kept_low == index else f" | WARNING 0x3603 readback=0x{kept_low:02X}" if kept_low is not None else " | readback NACK"))
            if abs(delta) > 10.0:
                hits.append((f"index {index}", delta, mean_low, mean_high))
        probe.log("")

        probe.log("step 5: final restore (index 7, 0 dB) and sanity check")
        probe.field_write(GAIN_INDEX, 0xFF)
        final = measure_or_restart(preview, probe, args.settle, args.window, "final restore")
        probe.log(f"final raw_mean={final:.2f}\n")

        probe.log("== REPORT (candidates sorted by |delta|, hit threshold 10 percent) ==")
        if not hits:
            probe.log("no candidate moved raw_mean beyond the threshold; "
                      "next steps: write-retention diff via LD_PRELOAD trace, or vendor register map")
        for name, delta, mean_low, mean_high in sorted(hits, key=lambda h: -abs(h[1])):
            probe.log(f"HIT {name}: low={mean_low:.2f} high={mean_high:.2f} delta={delta:+.1f} percent")
        return 0
    finally:
        preview.stop()
        stamp = time.strftime("%Y%m%d_%H%M%S")
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"probe_result_{stamp}.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(probe.log_lines) + "\n")
        print(f"log saved: {path}", flush=True)


if __name__ == "__main__":
    sys.exit(main())

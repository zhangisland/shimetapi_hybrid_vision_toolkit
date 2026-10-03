#!/usr/bin/env python3
"""probe_exposure_3a05.py — focused confirmation of the APS integration-time register.

Background: the LD_PRELOAD i2c-dev trace (passive_i2c_trace.c) captured the vendor
libapx003cc.so init sequence verbatim. It writes a 0x3Axx timing block at the very
end of init, then pulses the latch (0x342C = 1 then 0), then starts the stream
(0x340C = 1). Inside that block:

    0x3A05 = 0x05, 0x3A06 = 0xDC   ->   0x05DC = 1500   (16-bit, high byte first)

1500 "lines" is the strongest APS integration-time candidate and matches the value
flagged in APX003CC_PRIOR_KNOWLEDGE.md. probe_aps_exposure_v2.py wrote these bytes
WITHOUT the latch pulse, which is why it reported "flat".

This probe writes each candidate value, pulses the latch, then measures raw_mean
(the preview's per-frame mean) with raw_sample_sequence freshness tracking so a
stalled preview can never fake a sample.

Run on the board, bright scene in front of the APS lens:
    python3 tools/apx003cc_diagnostics/probe_exposure_3a05.py
    python3 tools/apx003cc_diagnostics/probe_exposure_3a05.py --all   # + 0x3A0C/0D, 0x3A14/15, 0x3A21/22

Interpretation: if raw_mean scales ~linearly with the 16-bit value, that register
IS the APS integration time and is directly writable at runtime.
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
LATCH = 0x342C

# (label, base-reg, candidate values in "lines"); high byte at base, low at base+1.
PRIMARY = [
    ("0x3A05/06 (integration?)", 0x3A05, [1500, 750, 375, 180]),
]
SECONDARY = [
    ("0x3A0C/0D", 0x3A0C, [4000, 2000]),
    ("0x3A14/15", 0x3A14, [1055, 527]),
    ("0x3A21/22", 0x3A21, [1600, 800]),
]


class Probe:
    def __init__(self, bus, addr):
        self.bus, self.addr = bus, addr
        self.lines = []

    def log(self, text):
        print(text, flush=True)
        self.lines.append(text)

    def write_reg(self, reg, val):
        cmd = ["i2ctransfer", "-y", "-f", str(self.bus),
               "w3@" + self.addr,
               f"0x{reg >> 8:02x}", f"0x{reg & 0xFF:02x}", f"0x{val:02x}"]
        return subprocess.run(cmd, capture_output=True, text=True).returncode == 0

    def read_reg(self, reg):
        cmd = ["i2ctransfer", "-y", "-f", str(self.bus),
               "w2@" + self.addr, f"0x{reg >> 8:02x}", f"0x{reg & 0xFF:02x}", "r1"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return None
        m = re.search(r"0x([0-9a-fA-F]{2})", r.stdout)
        return int(m.group(1), 16) if m else None

    def write_reg16(self, reg, val):
        """Write 16-bit big-endian: lower address holds the HIGH byte."""
        return (self.write_reg(reg, (val >> 8) & 0xFF)
                and self.write_reg(reg + 1, val & 0xFF))

    def latch(self):
        """Vendor applies timing regs with 0x342C = 1 then 0 (see trace)."""
        self.write_reg(LATCH, 0x01)
        self.write_reg(LATCH, 0x00)


class Preview:
    def __init__(self, width):
        self.width = width
        self.proc = None
        self.samples = deque(maxlen=64)
        self.last_seq = -1
        self.restarts = 0

    def start(self):
        cmd = [sys.executable, "hvs.py", "live", "--x5-vin-bypass", "--",
               "--preview-width", str(self.width), "--no-verify"]
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            m = re.search(r"raw_mean=([0-9.]+).*raw_sample_sequence=(\d+)", line)
            if m:
                seq = int(m.group(2))
                if seq > self.last_seq:          # only fresh frames
                    self.last_seq = seq
                    self.samples.append((time.monotonic(), float(m.group(1))))
            elif line.startswith(("Preview APS", "+ ")):
                pass
            else:
                print("  [preview] " + line.rstrip(), flush=True)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def ensure(self):
        if not self.alive():
            if self.restarts >= 3:
                raise RuntimeError("preview died 3 times; aborting")
            self.restarts += 1
            self.log(f"  preview down, restart {self.restarts}/3")
            self.start()

    def measure(self, settle, window):
        self.samples.clear()
        self.last_seq = -1
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
        v = preview.measure(settle, window)
        if v is not None:
            return v
        probe.log(f"  measurement lost during {what}; restarting preview")
    raise RuntimeError(f"cannot measure {what}")


def run_candidate(preview, probe, label, reg, values, settle, window):
    originals = [probe.read_reg(reg), probe.read_reg(reg + 1)]
    if any(v is None for v in originals):
        probe.log(f"{label}: read-back failed, skipped")
        return
    orig16 = (originals[0] << 8) | originals[1]
    probe.log(f"{label}: original=0x{orig16:04X} ({orig16} lines)")
    ref = None
    try:
        for val in values:
            probe.write_reg16(reg, val)
            probe.latch()
            mean = measure_or_restart(preview, probe, settle, window, f"{label}={val}")
            if ref is None:
                ref = mean
            ratio = (mean / ref * 100.0) if ref else 0.0
            probe.log(f"  write {val:5d} (0x{val:04X}): raw_mean={mean:8.2f}  ratio={ratio:6.1f}%")
    finally:
        probe.write_reg16(reg, orig16)
        probe.latch()
        back = measure_or_restart(preview, probe, settle, window, f"{label} restore")
        probe.log(f"  restore {orig16}: raw_mean={back:.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--i2c-bus", type=int, default=6)
    ap.add_argument("--i2c-address", default="0x3c")
    ap.add_argument("--preview-width", type=int, default=640)
    ap.add_argument("--settle", type=float, default=1.5)
    ap.add_argument("--window", type=float, default=2.0)
    ap.add_argument("--all", action="store_true",
                    help="also test secondary 0x3A0C/0D, 0x3A14/15, 0x3A21/22")
    args = ap.parse_args()

    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("preview needs a display; run from the board desktop session")

    probe = Probe(args.i2c_bus, args.i2c_address)
    preview = Preview(args.preview_width)
    candidates = PRIMARY + (SECONDARY if args.all else [])
    try:
        probe.log(f"== probe_exposure_3a05 bus={args.i2c_bus} addr={args.i2c_address} ==")
        probe.log("starting preview (--no-verify, no gain -> process never writes I2C)")
        preview.start()
        base = measure_or_restart(preview, probe, 4.0, args.window, "baseline")
        probe.log(f"baseline raw_mean={base:.2f}\n")
        for label, reg, values in candidates:
            run_candidate(preview, probe, label, reg, values, args.settle, args.window)
            probe.log("")
        probe.log("== done ==")
        probe.log("If raw_mean scales ~linearly with the written value, that register "
                  "is the APS integration time and is runtime-writable.")
        return 0
    finally:
        preview.stop()
        stamp = time.strftime("%Y%m%d_%H%M%S")
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            f"confirm_3a05_{stamp}.log")
        with open(path, "w", encoding="utf-8") as h:
            h.write("\n".join(probe.lines) + "\n")
        print(f"log saved: {path}", flush=True)


if __name__ == "__main__":
    sys.exit(main())

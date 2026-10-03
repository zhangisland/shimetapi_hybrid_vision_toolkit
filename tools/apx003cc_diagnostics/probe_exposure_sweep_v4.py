#!/usr/bin/env python3
"""APX003CC exposure sweep v4: classify 0x3502 bits + targeted register-space scan.

State after v3 (confirm_result_20261004_023633.log):
  - 0x3603 index port CLOSED: indices 0..6 at code 0x10 reproduce the 24 dB
    level exactly (7/7 gain alias); 0x00 state is deterministic (max gain).
  - 0x3500/01/02: only 0x3502 has optical effect, with two plateaus
    {0x00,0x03} -> 71.3 and {0x1F} -> 93.5; write masks are 01/01/1F
    (7 usable bits total) -> looks like mode flags, not a counter.

This script decides the remaining questions:
  phase 1: full 5-bit sweep of 0x3502 (0x00..0x1F). Four or more distinct,
           monotonic levels = graded control worth wiring up; a few plateaus
           = mode bits, drop it.
  phase 2: extract every register address the vendor init tables ever touch
           from /usr/hobot/lib/sensor/libapx003cc.so*.data section (8-byte
           records {u16 addr, pad, u16 data, pad}, region 0x30b0-0xc7d0),
           then hold 0xFF on each address (minus the protected skip list)
           long enough for one telemetry sample, restore, and compare against
           the rolling baseline. Addresses that reject the write (readback
           unchanged) are skipped without waiting.
  phase 3: flagged addresses get an A/B confirmation (0x00 vs 0xFF vs original,
           with retention audit).

Run on the board, toolkit root, desktop session, bright scene:
    python3 tools/apx003cc_diagnostics/probe_exposure_sweep_v4.py
"""
import argparse
import glob
import os
import re
import signal
import struct
import subprocess
import sys
import threading
import time
from collections import deque

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAIN_INDEX = 7
LATCH = 0x342C
INTEGRATION_REGS = (0x3500, 0x3501, 0x3502)
# Never write these: stream control, chip id, latch, gain port + handshake,
# EVS/mode discriminator, and the registers covered by phase 1.
SKIP = {0x340C, 0x3428, 0x342C, 0x3500, 0x3501, 0x3502,
        0x3600, 0x3601, 0x3602, 0x3603, 0x3660, 0x3661, 0x3662, 0x3804}
SO_GLOB = "/usr/hobot/lib/sensor/libapx003cc.so*"
DATA_REGION = (0x30B0, 0xC7D0)


def extract_vendor_addresses(path):
    with open(path, "rb") as handle:
        data = handle.read()
    start, end = DATA_REGION
    addrs = set()
    for offset in range(start, min(end, len(data) - 8), 8):
        addr, pad1, _value, pad2 = struct.unpack_from("<4H", data, offset)
        if pad1 == 0 and pad2 == 0 and addr:
            addrs.add(addr)
    return sorted(a for a in addrs if not (a & 0xFF00) == 0x0100)


class Probe:
    def __init__(self, bus, addr_hex):
        self.bus, self.addr = bus, addr_hex
        self.log_lines = []

    def log(self, text):
        print(text, flush=True)
        self.log_lines.append(text)

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

    def restore(self, reg, val):
        for _ in range(3):
            self.write_reg(reg, val)
            if self.read_reg(reg) == val:
                return True
        return False


class Preview:
    def __init__(self, width):
        self.width = width
        self.proc = None
        self.samples = deque(maxlen=64)
        self.total = 0
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
                self.total += 1
            elif not line.startswith(("Preview APS", "+ ")):
                print("  [preview] " + line.rstrip(), flush=True)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def ensure(self):
        if not self.alive():
            if self.restarts >= 5:
                raise RuntimeError("preview died five times; aborting")
            self.restarts += 1
            self.log(f"  preview not running (restart {self.restarts}/5); restarting")
            self.start()

    def marker(self):
        return self.total

    def wait_sample(self, marker, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.total > marker:
                return self.samples[-1][1]
            time.sleep(0.05)
        return None

    def baseline(self, before_marker, count=3):
        older = [v for t, v in self.samples if v is not None][-count - 5:-2]
        return sum(older) / len(older) if len(older) >= 2 else None

    def measure(self, settle, window):
        marker = self.marker()
        deadline = time.monotonic() + settle + window
        while time.monotonic() < deadline:
            if not self.alive():
                return None
            time.sleep(0.1)
        recent = [v for t, v in list(self.samples) if t >= deadline - window]
        return sum(recent) / len(recent) if recent else None

    def stop(self):
        if self.alive():
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.terminate()


def hold_and_measure(preview, probe, seconds, what):
    """Hold state long enough for >=1 telemetry sample; returns (value, marker)."""
    for attempt in range(2):
        preview.ensure()
        marker = preview.marker()
        value = preview.wait_sample(marker, timeout=seconds + 2.0)
        if value is not None:
            return value, marker
        probe.log(f"  telemetry lost during {what} (attempt {attempt + 1})")
    return None, marker


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--i2c-bus", type=int, default=6)
    parser.add_argument("--i2c-address", default="0x3c")
    parser.add_argument("--preview-width", type=int, default=640)
    parser.add_argument("--sensor-so", default=None, help="vendor sensor library path")
    args = parser.parse_args()

    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("preview needs a display; run from the board desktop session")
    probe = Probe(args.i2c_bus, args.i2c_address)
    preview = Preview(args.preview_width)
    try:
        probe.log(f"== probe_exposure_sweep_v4 bus={args.i2c_bus} addr={args.i2c_address} ==\n")
        preview.start()
        baseline = preview.measure(4.0, 2.0)
        probe.log(f"baseline raw_mean={baseline:.2f}\n")

        # ---- phase 1: 0x3502 five-bit sweep ----
        probe.log("phase 1: 0x3502 full writable sweep 0x00..0x1F (0x3500/01 held at factory)")
        probe.write_reg(0x3500, 0x01)
        probe.write_reg(0x3501, 0x00)
        levels = {}
        for value in range(0x00, 0x20):
            probe.write_reg(0x3502, value)
            raw, _ = hold_and_measure(preview, probe, 1.2, f"0x3502={value:02X}")
            if raw is None:
                continue
            levels.setdefault(round(raw, 1), []).append(value)
            probe.log(f"0x3502={value:02X}: raw_mean={raw:.2f}")
        probe.restore(0x3502, 0x03)
        distinct = sorted(levels)
        graded = len(distinct) >= 4 and distinct == sorted(distinct) and \
            (distinct[-1] - distinct[0]) > 10
        probe.log(f"phase 1 result: {len(distinct)} distinct levels {distinct} -> "
                  + ("GRADED response, candidate control" if graded else "plateau/mode-bit behavior, drop 0x3502 as exposure")
                  + "\n")

        # ---- phase 2: targeted sweep over vendor-touched addresses ----
        so_path = args.sensor_so
        if not so_path:
            candidates = sorted(glob.glob(SO_GLOB))
            if not candidates:
                raise RuntimeError(f"vendor sensor library not found: {SO_GLOB}")
            so_path = candidates[-1]
        addresses = [a for a in extract_vendor_addresses(so_path) if a not in SKIP]
        probe.log(f"phase 2: vendor table source={so_path}, {len(addresses)} unique addresses to sweep")
        flagged = []
        started = time.monotonic()
        for position, addr in enumerate(addresses):
            original = probe.read_reg(addr)
            if original is None:
                continue
            probe.write_reg(addr, 0xFF)
            accepted = probe.read_reg(addr)
            if accepted is None or accepted == original:
                probe.restore(addr, original)  # masked or write-only alias: nothing changed
                continue
            raw, marker = hold_and_measure(preview, probe, 1.2, f"0x{addr:04X}=0xFF")
            probe.restore(addr, original)
            after = probe.read_reg(addr)
            if raw is not None:
                base = preview.baseline(marker)
                if base:
                    delta = (raw - base) / base * 100.0
                    if abs(delta) > 10.0:
                        flagged.append((addr, original, raw, base, delta))
                        probe.log(f"FLAG 0x{addr:04X}: orig=0x{original:02X} probe raw_mean={raw:.2f}"
                                  f" baseline={base:.2f} delta={delta:+.1f} percent")
            if (position + 1) % 50 == 0:
                probe.log(f"  ... {position + 1}/{len(addresses)} addresses, {len(flagged)} flagged,"
                          f" {time.monotonic() - started:.0f}s elapsed")
        probe.log(f"phase 2 done: {len(flagged)} flagged of {len(addresses)}\n")

        # ---- phase 3: A/B confirmation on flagged addresses ----
        probe.log("phase 3: A/B confirmation (0x00 vs 0xFF vs original) on flagged addresses")
        confirmed = []
        for addr, original, _raw, _base, _delta in flagged:
            states = {}
            for probe_value in (0x00, 0xFF):
                probe.write_reg(addr, probe_value)
                raw, _ = hold_and_measure(preview, probe, 1.2, f"0x{addr:04X}={probe_value:02X}")
                states[probe_value] = raw
            probe.restore(addr, original)
            back, _ = hold_and_measure(preview, probe, 1.2, f"0x{addr:04X} restore")
            low, high = states.get(0x00), states.get(0xFF)
            if low and high and back and abs(high - low) / max(high, low) * 100.0 > 10.0 \
                    and abs(back - baseline) / baseline * 100.0 < 10.0:
                confirmed.append((addr, original, low, high))
                probe.log(f"CONFIRMED 0x{addr:04X}: orig=0x{original:02X} low=0x00 -> {low:.2f},"
                          f" high=0xFF -> {high:.2f}, restored -> {back:.2f}")
            else:
                probe.log(f"rejected 0x{addr:04X}: low={low} high={high} restored={back}")
        probe.log("")

        probe.log("== REPORT ==")
        probe.log(f"0x3502: {'GRADED control candidate' if graded else 'mode bits (not exposure)'}; levels={distinct}")
        if confirmed:
            for addr, original, low, high in confirmed:
                probe.log(f"exposure candidate CONFIRMED: 0x{addr:04X} (orig 0x{original:02X}),"
                          f" brightness {low:.2f} <-> {high:.2f}")
        else:
            probe.log("no confirmed exposure register in the vendor address set;"
                      " next step: LD_PRELOAD full I2C trace of the vendor stack (ground truth)")
        return 0
    finally:
        preview.stop()
        stamp = time.strftime("%Y%m%d_%H%M%S")
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"sweep_result_{stamp}.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(probe.log_lines) + "\n")
        print(f"log saved: {path}", flush=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("interrupted; partial log preserved", flush=True)
        sys.exit(130)

#!/usr/bin/env python3
"""APX003CC exposure follow-up: separate gain aliasing from a real exposure control.

Resolves the open questions left by probe_aps_exposure_v2 (2026-10-04 02:20 log):
  - 0x3603 readback = index mod 8 -> the selector is only 3 bits wide; distinct
    indices are 0..7 (7 = analog gain).
  - Indices 0..6 at 0x3602=0x00 all brightened ~8x while 0xFF returned exactly
    to baseline. Prime suspicion: the analog gain table maps smaller values to
    higher gain (0xFF=0 dB, 0x10=24 dB), so 0x00 may simply be max-gain
    aliasing, NOT exposure. Discriminator: writing 0x10 (the exact 24 dB code)
    at indices 0..6 must reproduce the 24 dB anchor level if (and only if) the
    field aliases gain.
  - 0x3500/3501/3502 (Sony-style coarse integration layout, factory 01 00 03)
    gave +23.8 percent with partial retention (readback clamped to 01 01 1F)
    -> byte-isolated sweep here to map the accepted range and response.

Run on the board, toolkit root, desktop session, bright scene:
    python3 tools/apx003cc_diagnostics/probe_exposure_confirm_v3.py
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
INTEGRATION_REGS = (0x3500, 0x3501, 0x3502)


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

    def write_regs(self, pairs):
        ok = all(self.write_reg(r, v) for r, v in pairs)
        return ok, [(r, v, self.read_reg(r)) for r, v in pairs]

    def latch(self):
        for value in (0, 1, 0):
            self.write_reg(LATCH, value)

    def field_write(self, index, value):
        self.write_reg(0x3603, index)
        self.write_reg(0x3660, 1)
        self.write_reg(0x3602, value)
        self.write_reg(0x3661, 1)
        self.write_reg(0x3662, 0)
        self.latch()


class Preview:
    def __init__(self, width):
        self.width = width
        self.proc = None
        self.aps = deque(maxlen=64)
        self.evs = deque(maxlen=64)
        self.restarts = 0

    def start(self):
        cmd = [sys.executable, "hvs.py", "live", "--x5-vin-bypass", "--",
               "--preview-width", str(self.width), "--no-verify"]
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            m_aps = re.search(r"raw_mean=([0-9.]+)", line)
            m_evs = re.search(r"evs_events_per_s=([0-9]+)", line)
            if m_aps:
                self.aps.append((time.monotonic(), float(m_aps.group(1))))
            if m_evs:
                self.evs.append((time.monotonic(), int(m_evs.group(1))))
            if not (line.startswith(("Preview APS", "+ ")) or m_aps or m_evs):
                print("  [preview] " + line.rstrip(), flush=True)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def ensure(self):
        if not self.alive():
            if self.restarts >= 3:
                raise RuntimeError("preview died three times; aborting")
            self.restarts += 1
            self.log(f"  preview not running (restart {self.restarts}/3); restarting")
            self.start()

    def measure(self, settle, window):
        self.aps.clear()
        self.evs.clear()
        deadline = time.monotonic() + settle + window
        while time.monotonic() < deadline:
            if not self.alive():
                return None, None
            time.sleep(0.1)
        aps = [v for t, v in self.aps if t >= deadline - window]
        evs = [v for t, v in self.evs if t >= deadline - window]
        aps_mean = sum(aps) / len(aps) if aps else None
        evs_now = evs[-1] if evs else None
        evs_gap = bool(evs) and any(v == 0 for v in evs)
        return aps_mean, (evs_now, evs_gap)

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
        aps, evs = preview.measure(settle, window)
        if aps is not None:
            note = ""
            if evs and evs[1]:
                note = " | WARNING EVS events dropped to 0 during window"
            return aps, note
        probe.log(f"  measurement lost during {what}; restarting preview")
    raise RuntimeError(f"cannot measure {what}; aborting")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--i2c-bus", type=int, default=6)
    parser.add_argument("--i2c-address", default="0x3c")
    parser.add_argument("--preview-width", type=int, default=640)
    parser.add_argument("--settle", type=float, default=1.5)
    parser.add_argument("--window", type=float, default=2.0)
    args = parser.parse_args()

    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("preview needs a display; run from the board desktop session")
    probe = Probe(args.i2c_bus, args.i2c_address)
    preview = Preview(args.preview_width)
    try:
        probe.log(f"== probe_exposure_confirm_v3 bus={args.i2c_bus} addr={args.i2c_address} ==\n")
        preview.start()
        baseline, note = measure_or_restart(preview, probe, 4.0, args.window, "baseline")
        probe.log(f"baseline raw_mean={baseline:.2f}{note}\n")

        probe.log("phase A: 24 dB anchor at index 7, code 0x10")
        probe.field_write(GAIN_INDEX, 0x10)
        anchor, note = measure_or_restart(preview, probe, args.settle, args.window, "anchor 24 dB")
        probe.log(f"anchor(24 dB) raw_mean={anchor:.2f}{note}")
        probe.field_write(GAIN_INDEX, 0xFF)
        probe.log("")

        probe.log("phase B: gain-alias discriminator, indices 0..6 at code 0x10")
        probe.log("if a field aliases the gain table, raw_mean must match the 24 dB anchor (within 15 percent)")
        alias_count = 0
        for index in range(0, 7):
            probe.field_write(index, 0x10)
            value, note = measure_or_restart(preview, probe, args.settle, args.window, f"index {index} @0x10")
            ratio = value / anchor if anchor else 0.0
            verdict = "GAIN-ALIAS" if 0.85 <= ratio <= 1.15 else "different"
            alias_count += verdict == "GAIN-ALIAS"
            probe.log(f"index {index} @0x10: raw_mean={value:.2f} ratio_vs_24dB={ratio:.2f} -> {verdict}{note}")
            probe.field_write(GAIN_INDEX, 0xFF)
        probe.log("")

        probe.log("phase C: reversibility at index 0, 0x00 -> 0xFF -> 0x00")
        probe.field_write(0, 0x00)
        first_low, note = measure_or_restart(preview, probe, args.settle, args.window, "index0 low #1")
        probe.field_write(0, 0xFF)
        mid, note = measure_or_restart(preview, probe, args.settle, args.window, "index0 high")
        probe.field_write(0, 0x00)
        second_low, note = measure_or_restart(preview, probe, args.settle, args.window, "index0 low #2")
        drift = (second_low - first_low) / max(first_low, 1e-9) * 100.0
        probe.log(f"low#1={first_low:.2f} high={mid:.2f} low#2={second_low:.2f} repeat-drift={drift:+.1f} percent"
                  f" -> {'deterministic' if abs(drift) < 5 else 'NOT deterministic (suspect artifact)'}{note}")
        probe.field_write(GAIN_INDEX, 0xFF)
        probe.log("")

        probe.log("phase D: 0x3500/01/02 (coarse integration layout) graded sweep with retention audit")
        originals = [(r, probe.read_reg(r)) for r in INTEGRATION_REGS]
        probe.log("factory: " + " ".join(f"0x{r:04X}=0x{v:02X}" for r, v in originals))
        orig_tuple = tuple(v for _, v in originals)
        configs = [orig_tuple, (0x01, 0x00, 0x1F), (0x01, 0x01, 0x1F), (0x01, 0x01, 0x00), (0x00, 0x00, 0x00)]
        seen = set()
        responses = []
        for config in configs:
            if config in seen:
                continue
            seen.add(config)
            probe.write_regs(list(zip(INTEGRATION_REGS, config)))
            accepted = tuple(probe.read_reg(r) for r in INTEGRATION_REGS)
            value, note = measure_or_restart(preview, probe, args.settle, args.window, f"cfg {config}")
            accepted_after = tuple(probe.read_reg(r) for r in INTEGRATION_REGS)
            stable = accepted == accepted_after
            probe.log(f"cfg={'(%02X %02X %02X)' % config}: accepted={'(%02X %02X %02X)' % accepted if all(a is not None for a in accepted) else 'NACK'}"
                      f" raw_mean={value:.2f} retained_after_measure={'yes' if stable else 'NO ' + ('(%02X %02X %02X)' % accepted_after)}{note}")
            responses.append((config, accepted, value))
        probe.write_regs(originals)
        restored, note = measure_or_restart(preview, probe, args.settle, args.window, "restore")
        probe.log(f"restored factory: raw_mean={restored:.2f}{note}\n")

        probe.log("== REPORT ==")
        probe.log(f"gain-alias at indices 0..6: {alias_count}/7 reproduced the 24 dB level -> "
                  + ("0x3602 at non-gain indices behaves like the gain table (NOT exposure)"
                     if alias_count >= 5 else "non-gain indices are NOT a gain alias; investigate individually"))
        if responses:
            best = max(responses, key=lambda r: r[2])
            probe.log(f"0x3500/01/02 strongest response: cfg={'(%02X %02X %02X)' % best[0]}"
                      f" accepted={'(%02X %02X %02X)' % best[1] if all(a is not None for a in best[1]) else 'NACK'}"
                      f" raw_mean={best[2]:.2f} (baseline {baseline:.2f}, x{best[2] / baseline:.2f})")
        probe.log("decision rule: exposure control confirmed only if 0x3500/01/02 shows graded,"
                  " retained, reversible response across at least three distinct levels")
        return 0
    finally:
        preview.stop()
        stamp = time.strftime("%Y%m%d_%H%M%S")
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"confirm_result_{stamp}.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(probe.log_lines) + "\n")
        print(f"log saved: {path}", flush=True)


if __name__ == "__main__":
    sys.exit(main())

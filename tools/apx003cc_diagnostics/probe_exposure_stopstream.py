#!/usr/bin/env python3
"""probe_exposure_stopstream.py — stop-stream exposure rewrite test.

Context (2026-10-04): runtime direct writes to 0x3503/04 while the APS stream
is live tear the frame timing: 920 lines showed a corrupted image, 640/240
produced NO frames at all (raw packets received=0). The init-time override of
the same values was clean (-12% brightness at 920), so the register identity
is right but the write timing is wrong. The vendor init trace ends with
0x340C 0x00 -> 0x01 (stream on), so this script tests the canonical sequence:

    stream OFF (0x340C=0) -> write 0x3503/04 -> stream ON (0x340C=1)

Run on the board desktop session (no build needed, no LD_PRELOAD):
    python3 tools/apx003cc_diagnostics/probe_exposure_stopstream.py

The preview window stays visible the whole time: WATCH IT at each step and
note whether the image is clean or torn. The script measures raw_mean and fps
per step; fps recovering to ~30 with clean scaling proves the path.
"""
import argparse
import ctypes
import fcntl
import os
import re
import signal
import subprocess
import sys
import threading
import time
from collections import deque

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# --- minimal i2c-dev combined-transaction access (repeated-start reads) ---
I2C_SLAVE = 0x0703
I2C_RDWR = 0x0707
I2C_M_RD = 0x0001

class i2c_msg(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("len", ctypes.c_uint16), ("buf", ctypes.POINTER(ctypes.c_ubyte))]

class i2c_rdwr_ioctl_data(ctypes.Structure):
    _fields_ = [("msgs", ctypes.POINTER(i2c_msg)), ("nmsgs", ctypes.c_uint32)]

class I2C:
    def __init__(self, bus, addr):
        self.addr = addr
        self.fd = os.open("/dev/i2c-%d" % bus, os.O_RDWR | os.O_CLOEXEC)
        fcntl.ioctl(self.fd, I2C_SLAVE, addr)

    def write8(self, reg, val):
        buf = (ctypes.c_ubyte * 3)((reg >> 8) & 0xFF, reg & 0xFF, val & 0xFF)
        msg = i2c_msg(addr=self.addr, flags=0, len=3, buf=buf)
        req = i2c_rdwr_ioctl_data(msgs=ctypes.pointer(msg), nmsgs=1)
        if fcntl.ioctl(self.fd, I2C_RDWR, req) != 1:
            raise RuntimeError("i2c write failed: reg=0x%04X" % reg)

    def read8(self, reg):
        rb = (ctypes.c_ubyte * 1)(0)
        wb = (ctypes.c_ubyte * 2)((reg >> 8) & 0xFF, reg & 0xFF)
        msgs = (i2c_msg * 2)(
            i2c_msg(addr=self.addr, flags=0, len=2, buf=wb),
            i2c_msg(addr=self.addr, flags=I2C_M_RD, len=1, buf=rb))
        req = i2c_rdwr_ioctl_data(msgs=msgs, nmsgs=2)
        if fcntl.ioctl(self.fd, I2C_RDWR, req) != 2:
            raise RuntimeError("i2c read failed: reg=0x%04X" % reg)
        return rb[0]

REG_STREAM_ON = 0x340C      # vendor trace: 0x00 -> 0x01 is the final stream-on write
REG_EXP_HI = 0x3503         # APS integration time, big-endian u16 (factory 1040)
REG_EXP_LO = 0x3504
FACTORY_LINES = 1040


class Preview:
    """Bootstrapped preview without gain args: the C++ side never touches I2C."""

    def __init__(self, width):
        self.width = width
        self.proc = None
        self.samples = deque(maxlen=64)
        self.last_seq = -1

    def start(self):
        cmd = [sys.executable, "hvs.py", "live", "--x5-vin-bypass", "--",
               "--preview-width", str(self.width), "--no-verify"]
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            sys.stderr.write("[preview] " + line)
            sys.stderr.flush()
            m = re.search(r"raw_mean=([0-9.]+).*raw_sample_sequence=(\d+)", line)
            if m:
                seq = int(m.group(2))
                if seq > self.last_seq:
                    self.last_seq = seq
                    self.samples.append((time.monotonic(), float(m.group(1))))

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def measure(self, settle, window):
        self.samples.clear()
        first_deadline = time.monotonic() + 30.0
        while time.monotonic() < first_deadline:
            if not self.alive():
                return None
            if self.samples:
                break
            time.sleep(0.1)
        if not self.samples:
            return None
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


def set_exposure(i2c, lines, verify=True):
    """stream OFF -> write 0x3503/04 -> readback -> stream ON."""
    i2c.write8(REG_STREAM_ON, 0x00)
    time.sleep(0.4)                      # let the in-flight frame drain
    i2c.write8(REG_EXP_HI, (lines >> 8) & 0xFF)
    i2c.write8(REG_EXP_LO, lines & 0xFF)
    got = ((i2c.read8(REG_EXP_HI) << 8) | i2c.read8(REG_EXP_LO)) if verify else None
    i2c.write8(REG_STREAM_ON, 0x01)
    time.sleep(0.5)
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--preview-width", type=int, default=960)
    ap.add_argument("--i2c-bus", type=int, default=6)
    ap.add_argument("--i2c-address", type=lambda s: int(s, 0), default=0x3C)
    ap.add_argument("--settle", type=float, default=2.0)
    ap.add_argument("--window", type=float, default=2.0)
    args = ap.parse_args()

    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("preview needs a display; run from the board desktop session")

    steps = [
        ("A", 920,  "-12% expect clean darker image"),
        ("B", 1040, "restore factory, expect baseline back"),
        ("C", 640,  "-38%"),
        ("D", 240,  "-77%"),
        ("E", 1040, "final restore"),
    ]

    i2c = I2C(args.i2c_bus, args.i2c_address)   # open only; first transaction waits for the sensor
    steps_note = None

    preview = Preview(args.preview_width)
    results = []
    base = None
    try:
        preview.start()
        base = preview.measure(args.settle, args.window)
        if base is None:
            raise RuntimeError("baseline failed: preview produced no fresh frames")
        bmean, bfps = base
        # Sensor is now powered and initialized by the vendor stack (first frames
        # streamed) - only NOW is the I2C address responsive. Reading 0x340C
        # before preview start caused Errno 121 (EIO, NACK on unpowered sensor).
        stream_before = i2c.read8(REG_STREAM_ON)
        print(f"== probe_exposure_stopstream ==\n"
              f"0x340C readback: 0x{stream_before:02X} (expect 0x01 = streaming)\n"
              f"baseline @factory 1040: raw_mean={bmean:.2f} fps~{bfps:.1f}\n"
              f"WATCH the preview window at every step: clean image = PASS, torn/garbage = FAIL\n",
              flush=True)

        for tag, lines, note in steps:
            got = set_exposure(i2c, lines)
            if got is not None and got != lines:
                print(f"[{tag}] READBACK MISMATCH: wrote {lines}, read back {got}", flush=True)
            res = preview.measure(args.settle, args.window)
            if res is None:
                print(f"[{tag}] {lines} lines: NO FRAMES after stream re-on ({note})", flush=True)
                results.append((tag, lines, None, None, note))
            else:
                mean, fps = res
                ratio = mean / bmean * 100.0
                print(f"[{tag}] {lines} lines: raw_mean={mean:.2f} ({ratio:.1f}% of base) "
                      f"fps~{fps:.1f}  ({note})", flush=True)
                results.append((tag, lines, mean, fps, note))

        print("\n== summary ==", flush=True)
        for tag, lines, mean, fps, note in results:
            if mean is None:
                print(f"  [{tag}] {lines:5d} lines: STREAM DEAD", flush=True)
            else:
                print(f"  [{tag}] {lines:5d} lines: raw_mean={mean:7.2f} fps~{fps:5.1f}  {note}", flush=True)
        print("\nRules:\n"
              "  1. Every step must show fps ~30 (stream survived stop/on) and the\n"
              "     preview image must be CLEAN (no tearing) - you watched it.\n"
              "  2. raw_mean must scale with lines: 920 -> ~88%, 640 -> ~62%, 240 -> ~23%.\n"
              "  3. All pass -> stop-stream rewrite is THE write path; wire it into\n"
              "     apx_manual.h::apply() (stream off / write / stream on).\n"
              "  4. If 920 is still torn -> needs group-hold or init-time write only.",
              flush=True)
    finally:
        if base is not None:
            try:
                set_exposure(i2c, FACTORY_LINES)
                print(f"\nrestored 0x3503/04 to factory {FACTORY_LINES} lines", flush=True)
            except Exception as exc:
                print(f"\nrestore failed: {exc}", flush=True)
        preview.stop()


if __name__ == "__main__":
    sys.exit(main())

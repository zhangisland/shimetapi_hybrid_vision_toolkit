#!/usr/bin/env python3
"""parse_i2c_trace.py — analyse passive_i2c_trace.so output.

Reads an I2C_WR trace log, reconstructs the vendor stack's real register-write
sequence, and cross-references it against:
  * the vendor .data init-table addresses (extracted from libapx003cc.so),
  * the register map we already know (gain chain, latch, stream control, etc.).

It does NOT sweep or write anything — read-only analysis. Output is a report
meant to surface (a) whether camera_reg_i2c_write8 covers the whole init
sequence or whether a block-write symbol (write_array) hid registers, and
(b) registers the vendor actually wrote that are NOT yet accounted for —
those are the remaining exposure/unknown-register candidates.

Usage (on the board):
    python3 tools/apx003cc_diagnostics/parse_i2c_trace.py /tmp/i2c_trace.log \
        [--sensor-so /usr/hobot/lib/sensor/libapx003cc.so.1.0.0]
"""
import argparse
import glob
import os
import re
import struct
import sys

WRITE_RE = re.compile(
    r"I2C_WR t=\d+\.\d+ bus=(\d+) width=(\d+) addr=0x([0-9a-fA-F]+) "
    r"reg=0x([0-9a-fA-F]+) value=0x([0-9a-fA-F]+) ret=(-?\d+)"
)

# Registers whose role is already established; these are expected, not surprises.
KNOWN = {
    0x340C: "stream ctrl (0=stop,1=run)",
    0x3428: "chip id (read)",
    0x342C: "latch pulse",
    0x3500: "flag bit (mask 0x01)",
    0x3501: "flag bit (mask 0x01)",
    0x3502: "bright-mode switch (mask 0x1F, latching)",
    0x3602: "gain LUT data",
    0x3603: "sub-field selector (3-bit, 7=gain)",
    0x3660: "gain handshake 1",
    0x3661: "gain handshake 1",
    0x3662: "gain handshake 0",
    0x3804: "EVS/mode discriminator",
}

DATA_REGION = (0x30B0, 0xC7D0)


def extract_vendor_addresses(path):
    with open(path, "rb") as f:
        data = f.read()
    start, end = DATA_REGION
    addrs = set()
    for off in range(start, min(end, len(data) - 8), 8):
        addr, pad1, _value, pad2 = struct.unpack_from("<4H", data, off)
        if pad1 == 0 and pad2 == 0 and addr:
            addrs.add(addr)
    return {a for a in addrs if (a & 0xFF00) != 0x0100}


def main():
    ap = argparse.ArgumentParser(description="parse passive_i2c_trace output")
    ap.add_argument("log", help="I2C_WR trace log path")
    ap.add_argument("--sensor-so", default=None)
    ap.add_argument("--top", type=int, default=40,
                    help="show at most this many unaccounted registers")
    args = ap.parse_args()

    writes = []          # (ts, bus, width, addr, reg, value, ret) in time order
    by_reg = {}          # reg -> ordered list of (value, ret)
    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = WRITE_RE.search(line)
            if not m:
                continue
            bus, width = int(m.group(1)), int(m.group(2))
            addr = int(m.group(3), 16)
            reg = int(m.group(4), 16)
            value = int(m.group(5), 16)
            ret = int(m.group(6))
            writes.append((bus, width, addr, reg, value, ret))
            by_reg.setdefault(reg, []).append((value, ret))

    print(f"== parsed {len(writes)} writes, {len(by_reg)} distinct registers ==")

    if not writes:
        print("NO I2C_WR lines found. Either the vendor stack did not call")
        print("camera_reg_i2c_write8 (block-write path?), or the trace .so was")
        print("not actually preloaded. Check stderr for the APX_TRACE banner.")
        return 1

    # Distinct values per register (a register written to several values may be
    # a dynamic control, e.g. exposure/AE, rather than a one-shot init constant).
    print("\n== registers written, by write count ==")
    rows = sorted(by_reg.items(), key=lambda kv: -len(kv[1]))
    for reg, vals in rows:
        distinct = sorted({v for v, _ in vals})
        known = KNOWN.get(reg)
        tag = f"  # {known}" if known else ""
        print(f"0x{reg:04X}: {len(vals):4d} writes, values={[hex(v) for v in distinct]}{tag}")

    # Cross-reference with the vendor init-table addresses.
    so = args.sensor_so
    if not so:
        cands = sorted(glob.glob("/usr/hobot/lib/sensor/libapx003cc.so*"))
        so = cands[-1] if cands else None
    if so and os.path.exists(so):
        init_addrs = extract_vendor_addresses(so)
        traced = set(by_reg)
        covered = init_addrs & traced
        uncovered = init_addrs - traced
        surprise = traced - init_addrs - set(KNOWN)
        print(f"\n== cross-reference vs vendor .data init table ({so}) ==")
        print(f"init-table addresses: {len(init_addrs)}")
        print(f"traced & in-table   : {len(covered)}")
        print(f"in-table but NOT traced (write_array path?): {len(uncovered)}")
        if uncovered:
            print("  untraced: " + " ".join(f"0x{a:04X}" for a in sorted(uncovered)[:80]))
        print(f"traced but NOT in-table and NOT known: {len(surprise)}")
        for reg in sorted(surprise)[:args.top]:
            vals = by_reg[reg]
            distinct = sorted({v for v, _ in vals})
            print(f"  UNACCOUNTED 0x{reg:04X}: {len(vals)} writes, values={[hex(v) for v in distinct]}")
    else:
        print(f"\n[sensor .so not found: {so}; skipping cross-reference]")

    # Time-ordered sequence (first N and last N) for eyeballing the init phase.
    print(f"\n== time-ordered sequence (first 60 writes) ==")
    for bus, width, addr, reg, value, ret in writes[:60]:
        known = KNOWN.get(reg)
        print(f"  bus={bus} w={width} addr=0x{addr:02x} reg=0x{reg:04X} val=0x{value:02X}"
              + (f"  # {known}" if known else ""))

    if len(writes) > 60:
        print(f"  ... ({len(writes) - 60} more)")

    return 0


if __name__ == "__main__":
    sys.exit(main())

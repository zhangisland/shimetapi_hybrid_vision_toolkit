#!/usr/bin/env python3
"""parse_i2c_trace.py — analyse passive_i2c_trace.so output (i2c-dev interposer).

Reads I2C_WR / I2C_RD / I2C_ADDR lines, reconstructs the vendor stack's real
register-write sequence, and cross-references it against the vendor .data
init-table addresses and the register map we already know.

Handles BOTH single-byte writes (camera_reg_i2c_write8) and burst/block writes
(write_array): a multi-byte `data=` field is expanded into consecutive
registers reg, reg+1, reg+2, ... so the two paths yield one uniform view.

Read-only analysis — never writes or sweeps anything.

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
    r"I2C_WR fd=\d+ addr=0x([0-9a-fA-F]+) reg=0x([0-9a-fA-F]+) data=([0-9a-fA-F ]*)"
)
ADDR_RE = re.compile(
    r"I2C_ADDR fd=\d+ addr=0x([0-9a-fA-F]+) reg=0x([0-9a-fA-F]+)"
)

# Timestamped write: the interposer prefixes every line with [sec.usec], so the
# dense init burst can be separated from later AE/runtime writes by time.
TS_WRITE_RE = re.compile(
    r"\[(\d+)\.(\d{6})\] I2C_WR fd=\d+ addr=0x[0-9a-fA-F]+ "
    r"reg=0x([0-9a-fA-F]+) data=([0-9a-fA-F ]*)"
)

# Registers whose role is already established.
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


def parse_bytes(text):
    return [int(b, 16) for b in text.split()]


def main():
    ap = argparse.ArgumentParser(description="parse passive_i2c_trace output")
    ap.add_argument("log", help="trace log path")
    ap.add_argument("--sensor-so", default=None)
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--runtime", action="store_true",
                    help="print writes after the init burst (AE/runtime exposure)")
    args = ap.parse_args()

    by_reg = {}   # reg -> ordered list of values (single-byte writes, burst-expanded)
    burst = []    # (reg, [bytes]) for multi-byte writes
    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()
    events = []  # (t, reg, [bytes]) time-ordered, for --runtime
    for m in TS_WRITE_RE.finditer(content):
        t = int(m.group(1)) + int(m.group(2)) / 1e6
        events.append((t, int(m.group(3), 16), parse_bytes(m.group(4))))
    # The interposer may emit entries without trailing newlines (all on one line);
    # scan the whole content with finditer so every write is counted, not just the
    # first match per line.
    for m in WRITE_RE.finditer(content):
        reg = int(m.group(2), 16)
        data = parse_bytes(m.group(3))
        if len(data) == 1:
            by_reg.setdefault(reg, []).append(data[0])
        elif len(data) > 1:
            burst.append((reg, data))
            for i, b in enumerate(data):
                by_reg.setdefault(reg + i, []).append(b)
    for a in ADDR_RE.finditer(content):
        # bare 2-byte address write (read preamble); record for coverage
        by_reg.setdefault(int(a.group(2), 16), [])

    if not by_reg and not burst:
        print("NO I2C_WR lines. The vendor stack did not issue i2c-dev writes")
        print("in this run (v4l2-subdev path?), or the trace .so was not preloaded.")
        print("Check stderr for APX_TRACE_LOADED and /dev/i2c OPEN lines.")
        return 1

    print(f"== parsed {sum(len(v) for v in by_reg.values())} register writes, "
          f"{len(by_reg)} distinct registers, {len(burst)} burst writes ==")

    if args.runtime and events:
        events.sort(key=lambda e: e[0])
        # The init burst is one dense cluster; the largest inter-write gap
        # separates it from any later AE/runtime traffic.
        best_gap, split = 0.0, 0
        for i in range(1, len(events)):
            g = events[i][0] - events[i - 1][0]
            if g > best_gap:
                best_gap, split = g, i
        init_end = events[split - 1][0] if split else events[-1][0]
        init_val = {}
        for t, reg, data in events[:split]:
            if len(data) == 1:
                init_val[reg] = data[0]
        print(f"\n== runtime writes (init burst ends t={init_end:.3f}s, "
              f"max gap {best_gap:.3f}s) ==")
        rt = events[split:]
        if not rt:
            print("  NONE — no register writes after the init burst.")
            print("  (AE did not move: run longer, or do the cover/uncover maneuver)")
        else:
            for t, reg, data in rt:
                if len(data) == 1:
                    v = data[0]
                    prev = init_val.get(reg)
                    note = "" if prev is None or prev == v else \
                        f"   (changed from 0x{prev:02X})"
                    print(f"  t={t:8.3f}s  0x{reg:04X} = 0x{v:02X}{note}")
                    init_val[reg] = v
                else:
                    print(f"  t={t:8.3f}s  0x{reg:04X} +{len(data)} bytes "
                          + " ".join(f"{b:02X}" for b in data[:8]))

    if burst:
        print(f"\n== burst/block writes ({len(burst)}) ==")
        for reg, data in burst[:40]:
            print(f"  0x{reg:04X} +{len(data)}: "
                  + " ".join(f"{b:02X}" for b in data[:16])
                  + ("..." if len(data) > 16 else ""))

    print("\n== registers written, by write count ==")
    rows = sorted(by_reg.items(), key=lambda kv: -len(kv[1]))
    for reg, vals in rows:
        distinct = sorted({v for v in vals})
        known = KNOWN.get(reg)
        tag = f"  # {known}" if known else ""
        print(f"0x{reg:04X}: {len(vals):4d} writes, values={[hex(v) for v in distinct]}{tag}")

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
        print(f"in-table but NOT traced: {len(uncovered)}")
        if uncovered:
            print("  untraced: " + " ".join(f"0x{a:04X}" for a in sorted(uncovered)[:100]))
        print(f"traced but NOT in-table and NOT known: {len(surprise)}")
        for reg in sorted(surprise)[:args.top]:
            vals = by_reg[reg]
            distinct = sorted({v for v in vals})
            print(f"  UNACCOUNTED 0x{reg:04X}: {len(vals)} writes, values={[hex(v) for v in distinct]}")
    else:
        print(f"\n[sensor .so not found: {so}; skipping cross-reference]")

    return 0


if __name__ == "__main__":
    sys.exit(main())

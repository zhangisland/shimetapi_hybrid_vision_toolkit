#!/usr/bin/env python3
"""Patch the APS timing register pair 0x3503/0x3504 in ALL 7 HVS init tables
of libapx003cc.so.1.0.0 (record layout: addr u16, pad u16, value u16, pad u16;
sensor write uses value & 0xFF per register).

Board evidence (2026-10-05): patching 0x3504 0x10->0x98 in all 7 HVS tables
gives a CLEAN ~2.7x brightening (raw_mean 125 -> 338), so init-time table
patching is a working write path. The register pair is NOT a plain
integration-line counter (0x498 vs 0x410 is only +13% numeric) - flag bits
are suspected; probe bit by bit.

Usage:
  python3 patch_hvs_exposure.py 0x0498     # set pair to 0x0498 (0x3503=0x04, 0x3504=0x98)
  python3 patch_hvs_exposure.py --factory  # restore from the .bak.bak factory copy

Only the HVS tables are touched (the linear table at 0x003ac8 is left alone).
"""
import struct
import sys

SO = '/usr/hobot/lib/sensor/libapx003cc.so.1.0.0'
FACTORY = '/usr/hobot/lib/sensor/libapx003cc.so.1.0.0.bak.bak'

# record start offsets in the 7 HVS tables
REC_3503 = [0x004cc8, 0x005f30, 0x0071a0, 0x008410, 0x009678, 0x00a8e0, 0x00bb48]
REC_3504 = [0x004cd0, 0x005f38, 0x0071a8, 0x008418, 0x009680, 0x00a8e8, 0x00bb50]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    arg = sys.argv[1]
    if arg == '--factory':
        import shutil
        shutil.copyfile(FACTORY, SO)
        print('restored factory from', FACTORY)
        return

    value = int(arg, 0)
    hi, lo = (value >> 8) & 0xFF, value & 0xFF

    d = bytearray(open(SO, 'rb').read())
    for recs, reg, val in ((REC_3503, 0x3503, hi), (REC_3504, 0x3504, lo)):
        for rec in recs:
            addr, pad1, cur, pad2 = struct.unpack_from('<4H', d, rec)
            assert addr == reg and pad1 == 0 and pad2 == 0, \
                f'record 0x{rec:x} malformed (addr=0x{addr:x})'
            struct.pack_into('<H', d, rec + 4, val)
    open(SO, 'wb').write(d)

    chk = open(SO, 'rb').read()
    ok3 = [struct.unpack_from('<H', chk, r + 4)[0] for r in REC_3503]
    ok4 = [struct.unpack_from('<H', chk, r + 4)[0] for r in REC_3504]
    print(f'verify 0x3503={hi:#04x} in all 7 tables: {[hex(v) for v in ok3]}')
    print(f'verify 0x3504={lo:#04x} in all 7 tables: {[hex(v) for v in ok4]}')
    print('now restart the preview (hvs.py live ...) to load the patched table')


if __name__ == '__main__':
    main()

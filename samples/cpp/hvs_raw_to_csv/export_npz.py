#!/usr/bin/env python3
"""Stream SDK-decoded columns into a compressed NumPy archive; standard library only."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

COLUMNS = (("x", "<u2", 2), ("y", "<u2", 2),
           ("polarity", "|u1", 1), ("timestamp", "<i8", 8))

class Progress:
    def __init__(self, total):
        self.total = total
        self.start = time.monotonic()
        self.last = 0

    def update(self, done, final=False):
        now = time.monotonic()
        if not final and now - self.last < 0.25:
            return
        self.last = now
        ratio = min(done / self.total, 1) if self.total else 1
        if not final:
            ratio = min(ratio, 0.999)
        filled = int(ratio * 30)
        eta = f"{(now-self.start)*(self.total-done)/done:.0f}s" if done else "--"
        print(f"\r[{'='*filled}{' '*(30-filled)}] {ratio*100:5.1f}%  ETA {eta}          ",
              end="\n" if final else "", file=sys.stderr, flush=True)

def npy_header(dtype, count):
    # NPY 1.0: little-endian uint16 header length, ASCII dict, 64-byte alignment.
    header = repr(dict(descr=dtype, fortran_order=False, shape=(count,))).encode('ascii')
    header += b' ' * ((-(10 + len(header) + 1)) % 64) + b'\n'
    return b'\x93NUMPY\x01\x00' + struct.pack('<H', len(header)) + header

def compress_columns(directory, output):
    count = (directory / 'polarity').stat().st_size
    for name, _, size in COLUMNS:
        if (directory / name).stat().st_size != count * size:
            raise RuntimeError('Inconsistent decoded column sizes')
    progress = Progress(count * 13)
    done = 0
    # Exclusive creation protects recordings/previous exports even under races.
    with output.open('xb') as destination:
        try:
            with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED,
                                 compresslevel=6, allowZip64=True) as archive:
                for name, dtype, _ in COLUMNS:
                    with archive.open(name + '.npy', 'w', force_zip64=True) as member:
                        member.write(npy_header(dtype, count))
                        with (directory / name).open('rb') as source:
                            while True:
                                block = source.read(1024 * 1024)
                                if not block:
                                    break
                                member.write(block)
                                done += len(block)
                                progress.update(done)
            destination.flush()
        except BaseException:
            destination.close()
            output.unlink()
            raise
    progress.update(done, final=True)

def export(decoder, source, output):
    if output.exists():
        raise RuntimeError('Output exists; choose a NEW NPZ file')
    if not source.is_file() or not source.stat().st_size:
        raise RuntimeError('Missing or empty input events.raw')
    with tempfile.TemporaryDirectory(prefix='.hvs-npz-', dir=output.parent) as temporary:
        columns = Path(temporary) / 'columns'
        print('Decode', file=sys.stderr, flush=True)
        subprocess.run([str(decoder), str(source), str(columns), '--columns'], check=True)
        print('Compress', file=sys.stderr, flush=True)
        compress_columns(columns, output)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    export(args.decoder, args.input, args.output)

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)

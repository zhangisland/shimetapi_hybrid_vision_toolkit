#!/usr/bin/env python3
"""Read-only AVI header vs host receive timestamp validation (not a decode test)."""
import argparse
import csv
import json
import math
from pathlib import Path
import struct


def check(folder):
    folder = Path(folder)
    with (folder / 'aps.frames.csv').open(newline='') as source:
        rows = list(csv.DictReader(source))
    if len(rows) < 2:
        raise ValueError('Need at least two APS timestamps to validate time scale')
    stamps = [int(row['host_receive_ns']) for row in rows]
    if any(int(row['index']) != i for i, row in enumerate(rows)):
        raise ValueError('Non-contiguous recorded frame indices')
    if any(b <= a for a, b in zip(stamps, stamps[1:])):
        raise ValueError('Receive timestamps are not strictly increasing')
    span = (stamps[-1] - stamps[0]) / 1e9
    expected_fps = (len(stamps) - 1) / span
    path = folder / 'aps.avi'
    size = path.stat().st_size
    videos = []
    with path.open('rb') as source:
        def read(at, n):
            source.seek(at)
            data = source.read(n)
            if len(data) != n:
                raise ValueError('Truncated AVI')
            return data
        if read(0, 4) != b'RIFF' or read(8, 4) != b'AVI ':
            raise ValueError('Not a RIFF AVI')
        if struct.unpack('<I', read(4, 4))[0] + 8 != size:
            raise ValueError('Unfinalized AVI size')
        def walk(start, end, depth=0):
            if depth > 3:
                raise ValueError('Invalid AVI nesting')
            pos = start
            while pos + 8 <= end:
                tag = read(pos, 4)
                n = struct.unpack('<I', read(pos + 4, 4))[0]
                nxt = pos + 8 + n + (n & 1)
                if nxt > end:
                    raise ValueError('Invalid AVI chunk size')
                if tag == b'LIST' and n >= 4 and read(pos + 8, 4) in (b'hdrl', b'strl'):
                    walk(pos + 12, pos + 8 + n, depth + 1)
                elif tag == b'strh' and n >= 56 and read(pos + 8, 4) == b'vids':
                    scale, rate, _, count = struct.unpack('<IIII', read(pos + 8 + 20, 16))
                    videos.append((scale, rate, count))
                pos = nxt
        walk(12, size)
    if len(videos) != 1:
        raise ValueError('Expected exactly one video stream')
    scale, rate, count = videos[0]
    if not scale or not rate or count != len(rows):
        raise ValueError('Invalid AVI rate or frame count differs from metadata')
    fps = rate / scale
    if not math.isclose(fps, expected_fps, rel_tol=0.005):
        raise ValueError(f'AVI fps {fps} differs from host average {expected_fps}')
    return dict(frames=count, avi_fps=fps, host_average_fps=expected_fps,
                receive_span_seconds=span, avi_duration_seconds=count / fps,
                end_frame_duration_seconds=1 / fps,
                largest_receive_gap_ms=max(b - a for a, b in zip(stamps, stamps[1:])) / 1e6,
                limitation='constant mean rate; irregular gaps remain in CSV; host timing, not hardware sync')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.session), indent=2))

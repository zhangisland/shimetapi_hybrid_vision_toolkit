#!/usr/bin/env python3
"""Read-only X5 runtime fingerprint. No I2C scans, writes, or system changes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int, help='Running recorder PID: verify actual mapped libraries')
    p.add_argument('--binary', type=Path, required=True)
    args = p.parse_args()
    paths = {args.binary.resolve()}
    report = {'binary': str(args.binary.resolve()), 'files': {}}
    if args.pid:
        maps = Path(f'/proc/{args.pid}/maps').read_text()
        report['cmdline'] = Path(f'/proc/{args.pid}/cmdline').read_bytes().replace(b'\0', b' ').decode()
        report['status'] = Path(f'/proc/{args.pid}/status').read_text()
        paths.update(Path(line.split()[-1]) for line in maps.splitlines()
                     if len(line.split()) >= 6 and line.split()[-1].startswith('/') and '.so' in line)
        report['maps'] = maps
    else:
        report['ldd_not_actual_maps'] = subprocess.run(['ldd', str(args.binary)], capture_output=True, text=True).stdout
    sensor = Path('/usr/hobot/lib/sensor/libapx003cc.so.1.0.0')
    if sensor.exists():
        paths.add(sensor)
    for path in sorted(paths):
        try:
            digest = hashlib.sha256()
            with path.open('rb') as f:
                for data in iter(lambda: f.read(1048576), b''):
                    digest.update(data)
            report['files'][str(path)] = digest.hexdigest()
        except OSError as e:
            report['files'][str(path)] = str(e)
    for path in ('/proc/swaps', '/proc/meminfo', '/proc/self/limits'):
        report[path] = Path(path).read_text()
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

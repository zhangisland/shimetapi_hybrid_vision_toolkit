#!/usr/bin/env python3
"""Audit native VIN timing and RAW10 brightness, without trusting AVI frame rate.

python3 tools/apx003cc_diagnostics/check_vin_recording.py SESSION [SESSION ...]
Requires numpy only for saved-image photometry. Diagnostic-only sessions need no
third-party module. Exit 2 means the timing/stream requirements were not met.
Brightness is a comparison aid, not proof of per-frame exposure.
"""
import argparse
import json
import hashlib
from pathlib import Path


def inspect(folder, minimum_fps=27):
    path = folder/'summary.txt'
    if not path.exists():
        path = folder/'diagnostic.txt'
    summary = dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)
    seconds = float(summary['capture_seconds'])
    received = int(summary['aps_received'])
    issues = []
    if summary.get('status') != 'complete':
        issues.append(summary.get('reason', 'capture failed'))
    if seconds <= 0 or received / seconds < minimum_fps:
        issues.append('APS effective-window rate below threshold')
    for key in ('aps_duplicate_ids', 'aps_id_resets', 'aps_frame_id_gaps',
                'aps_capacity_rejected', 'evs_capacity_rejected', 'evs_frame_id_gaps'):
        if int(summary.get(key, 0)):
            issues.append(f'{key}={summary[key]}')
    if int(summary['evs_received']) == 0:
        issues.append('EVS absent')
    result = {'session': str(folder), 'seconds': seconds,
              'aps_received': received, 'aps_window_fps': received/seconds if seconds else 0,
              'aps_interarrival_fps': float(summary['aps_receive_fps']),
              'evs_packets': int(summary['evs_received']),
              'evs_bytes_per_second': float(summary['evs_bytes_per_second']),
              'exposure_lines_config': summary.get('aps_exposure_submitted_lines'),
              'gain_db_config': summary.get('aps_gain_submitted_db'),
              'register_verification': summary.get('aps_register_verification'),
              'per_frame_exposure': 'unavailable', 'upstream_sensor_loss': 'unknown',
              'issues': issues, 'timing_pass': not issues}
    meta = folder/'vin.frames.jsonl'
    if meta.exists():
        import numpy as np
        rows = [json.loads(line) for line in meta.read_text().splitlines()]
        aps = [r for r in rows if r['stream'] == 'aps']
        evs = [r for r in rows if r['stream'] == 'evs']
        if len(aps) != int(summary['aps_frames']) or len(evs) != int(summary['evs_packets']):
            raise ValueError('Metadata count differs from saved count')
        if sum(r['bytes'] for r in aps) != (folder/'aps.vin.bin').stat().st_size:
            raise ValueError('Raw APS file size mismatch')
        if len(aps) != received:
            issues.append('received APS count differs from retained count')
        timestamps = [r.get('vin_timestamp', 0) for r in aps]
        result['vin_timestamp_nonzero_unique'] = len(set(t for t in timestamps if t))
        result['frame_id_unavailable'] = int(summary.get('aps_unavailable_ids', 0))
        result['photometry_warnings'] = []
        values, saturated, invalid = [], [], []
        interior, prefix, hashes = [], [], []
        with (folder/'aps.vin.bin').open('rb') as raw:
            # At most 20 uniformly distributed real frames, no AVI decoding.
            selected = sorted(set(np.linspace(0, len(aps)-1, min(20, len(aps)), dtype=int))) if aps else []
            for index in selected:
                row = aps[index]
                raw.seek(row['offset'])
                buf = raw.read(row['bytes'])
                image = np.ndarray((row['height'], row['width']), dtype='<u2',
                                   buffer=buf, strides=(row['stride'], 2))
                # Report, never silently reinterpret, the counter-like first word.
                prefix.append(int(image[0, 0]))
                interior.append(float(image.reshape(-1)[1:].mean()))
                hashes.append(hashlib.sha256(buf[2:]).hexdigest())
                values.append(float(image.mean()))
                saturated.append(float((image >= 1020).mean()*100))
                invalid.append(int((image > 1023).sum()))
        result.update(mean_raw10=float(np.mean(values)) if values else None,
                      saturated_percent=float(np.mean(saturated)) if saturated else None,
                      sampled_frames=len(values), words_outside_raw10=sum(invalid),
                      first_words=prefix,
                      mean_excluding_first_word=float(np.mean(interior)) if interior else None,
                      sampled_unique_payloads_excluding_first_word=len(set(hashes)),
                      preview='RAW unchanged; first word may be embedded metadata, not a pixel')
        if any(invalid):
            result['photometry_warnings'].append('Words outside low 10 bits; full-plane photometry unverified. Excluding-first-word mean is diagnostic only.')
        result['timing_pass'] = not issues
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sessions', nargs='+', type=Path)
    p.add_argument('--minimum-fps', type=float, default=27)
    args = p.parse_args()
    results = [inspect(folder, args.minimum_fps) for folder in args.sessions]
    print(json.dumps(results, indent=2, ensure_ascii=False))
    if len(results) >= 3:
        print('Compare mean_raw10 and saturated_percent under fixed illumination/aperture/gain. '
              'Register values are configuration snapshots, not per-frame exposure measurements.')
    return 0 if all(r['timing_pass'] for r in results) else 2


if __name__ == '__main__':
    raise SystemExit(main())

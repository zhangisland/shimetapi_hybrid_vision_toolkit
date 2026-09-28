#!/usr/bin/env python3
"""Capture an exposure sweep or compare existing native ISP NV12 recordings.
Uses Python standard library + ffmpeg. Never changes/deletes source recordings.
"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import sys

try:
    from .check_native_recording import check as check_timing
except ImportError:
    from check_native_recording import check as check_timing

ROOT = Path(__file__).resolve().parents[1]
NUMBER = r'([-+0-9.eE]+)'


def positive(text):
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError('Expected a positive finite number')
    return value


def read_attributes(log):
    result = dict(api_set_accepted=False, readback_us=None,
                  readback_again=None, readback_dgain=None)
    if log is None:
        return result
    text = Path(log).read_text(encoding='utf-8', errors='replace')
    result['api_set_accepted'] = 'ISP AE API accepted request' in text
    found = re.findall(r'ISP attribute readback[^\n]*seconds=' + NUMBER +
                       r' again=' + NUMBER + r' dgain=' + NUMBER, text)
    if found:
        seconds, again, dgain = map(float, found[-1])
        if all(math.isfinite(v) and v > 0 for v in (seconds, again, dgain)):
            result.update(readback_us=seconds * 1e6, readback_again=again, readback_dgain=dgain)
    return result


def pixel_stats(data):
    size = 64 * 48
    if not data or len(data) % size:
        raise ValueError('No complete decoded frames after warmup; check recording duration')
    means, lows, highs = [], [], []
    for offset in range(0, len(data), size):
        frame = data[offset:offset + size]
        means.append(sum(frame) / size)
        lows.append(sum(v <= 20 for v in frame) * 100 / size)
        highs.append(sum(v >= 235 for v in frame) * 100 / size)
    return dict(sample_frames=len(means), y_mean_median=statistics.median(means),
                y_mean_min=min(means), y_mean_max=max(means),
                y_mean_stdev=statistics.pstdev(means),
                y_le20_pct_median=statistics.median(lows),
                y_ge235_pct_median=statistics.median(highs))


def image_stats(session, warmup, sample_seconds):
    # Extract ISP Y directly, NOT a BGR->gray conversion. Spatial area reduction
    # means clipping fractions describe the reduced center ROI, not full pixels.
    command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin',
               '-ss', str(warmup), '-i', str(session / 'aps.avi'), '-t', str(sample_seconds),
               '-map', '0:v:0', '-an', '-vf',
               'crop=iw/2:ih/2:iw/4:ih/4,extractplanes=y,scale=64:48:flags=area',
               '-frames:v', '120', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1']
    decoded = subprocess.run(command, capture_output=True, timeout=90)
    if decoded.returncode:
        raise RuntimeError('ffmpeg decode failed: ' + decoded.stderr.decode(errors='replace'))
    return pixel_stats(decoded.stdout)


def analyze(entries, output, warmup=1.0, sample_seconds=3.0):
    if not shutil.which('ffmpeg'):
        raise RuntimeError('ffmpeg is required (ffprobe alone cannot decode pixels)')
    if len(entries) < 2:
        raise ValueError('At least two recordings are needed')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    rows = []
    for entry in entries:
        session = Path(entry['session']).resolve()
        summary = dict(line.split('=', 1) for line in (session / 'summary.txt').read_text().splitlines() if '=' in line)
        if summary.get('status') != 'complete' or summary.get('aps_storage_format') != 'ISP_NV12_uncorrected':
            raise ValueError(f'{session}: require complete native ISP NV12 recording, not Bayer bypass')
        timing = check_timing(session)
        if timing['receive_span_seconds'] <= warmup:
            raise ValueError(f'{session}: recording is shorter than warmup')
        row = dict(requested_us=entry['requested_us'], session=str(session),
                   requested_again=entry.get('requested_again'), requested_dgain=entry.get('requested_dgain'))
        row.update(read_attributes(entry.get('log')))
        row.update(image_stats(session, warmup, sample_seconds))
        row.update(frames=timing['frames'], fps=timing['avi_fps'],
                   duration_s=timing['avi_duration_seconds'],
                   largest_receive_gap_ms=timing['largest_receive_gap_ms'],
                   recording_queue_dropped=summary.get('queue_dropped', 'unknown'))
        rows.append(row)
    rows.sort(key=lambda row: row['requested_us'])
    baseline = rows[0]['y_mean_median']
    for row in rows:
        row['y_ratio_to_shortest'] = row['y_mean_median'] / baseline if baseline > 0 else None
    report = dict(rows=rows, warmup_seconds=warmup, sample_seconds=sample_seconds,
                  measurement='ISP Y center half-width/half-height ROI, area reduced to 64x48, max 120 frames',
                  actual_sensor_exposure_verified=False,
                  limitations=['Requested exposure labels are supplied by the caller; missing readback stays null.',
                               'API acceptance and ISP attribute readback are not per-frame sensor exposure.',
                               'Keep scene, illumination and gains fixed. Exclude moving subjects and saturated regions.',
                               'ISP gamma/tone processing and Y black offset prevent linear exposure-ratio claims.',
                               'Frame rate/count cannot validate exposure. Large Y variation may indicate flicker or scene changes.',
                               'A brightness response is evidence of imaging response, not a measured integration time.'])
    (output / 'comparison.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    with (output / 'comparison.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)
    print('requested_us  readback_us   median_Y  Y_ratio   near_white%   FPS   frames')
    for row in rows:
        print(f"{row['requested_us']:12g}  {str(row['readback_us']):>11}  {row['y_mean_median']:9.3f}  "
              f"{str(round(row['y_ratio_to_shortest'], 3)) if row['y_ratio_to_shortest'] is not None else 'unknown':>7}  "
              f"{row['y_ge235_pct_median']:11.2f}  {row['fps']:6.2f}  {row['frames']:6d}")
    print('Saved:', output)
    print('Readback is not actual sensor exposure. Compare Y statistics under fixed scene/lighting/gains.')
    return report


def active_cameras():
    # Do not stop any pre-existing process. This catches known sample processes,
    # not every possible SDK client; the operator must also check unknown clients.
    matches = []
    for path in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            first = path.read_bytes().split(b'\0')[0].decode(errors='replace')
            name = Path(first).name
            if name.startswith(('hv_sample_', 'hv_hvs_record', 'dual_vc_vin', 'sunrise_camera')):
                matches.append(f'{path.parent.name}: {name}')
        except (OSError, ValueError):
            continue
    return matches


def capture(args):
    if sys.platform != 'linux':
        raise RuntimeError('Capture must run on the X5 Linux board')
    if not shutil.which('ffmpeg'):
        raise RuntimeError('ffmpeg is required for the comparison step')
    if args.seconds <= args.warmup + args.sample_seconds:
        raise ValueError('--seconds must exceed --warmup + --sample-seconds')
    existing = active_cameras()
    if existing:
        raise RuntimeError('Camera processes already running; none were stopped: ' + '; '.join(existing))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    entries = []
    for index, exposure in enumerate(args.exposures_us):
        prefix = output / f'capture_{index}_{exposure:g}us'
        log = output / f'capture_{index}_{exposure:g}us.log'
        command = [sys.executable, str(ROOT / 'hvs.py'), 'live', '--build-dir', str(args.build_dir.resolve()), '--',
                   '--profile', str(args.profile), '--aps-exposure-us', str(exposure),
                   '--aps-gain', str(args.gain), '--aps-dgain', str(args.dgain),
                   '--aps-correction', 'off', '--aps-wb', 'off', '--no-display',
                   '--seconds', str(args.seconds), '--record', '--output', str(prefix)]
        print('Capturing exposure request', exposure, 'us; log:', log, flush=True)
        with log.open('w', encoding='utf-8') as stream:
            # Child owns its own group; interrupt only our child, never another camera process.
            child = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                code = child.wait(timeout=args.seconds + 90)
            except (KeyboardInterrupt, subprocess.TimeoutExpired):
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGINT)
                    try:
                        child.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError(f'Our capture PID {child.pid} did not stop; inspect it and {log}. No force-kill issued.')
                raise
        if code:
            raise RuntimeError(f'Capture failed ({code}); partial output retained. Read {log}')
        sessions = [p for p in output.glob(prefix.name + '_*') if p.is_dir()]
        if len(sessions) != 1:
            raise RuntimeError(f'Expected one recording directory; inspect {log}')
        entries.append(dict(requested_us=exposure, requested_again=args.gain, requested_dgain=args.dgain,
                            session=str(sessions[0]), log=str(log)))
        (output / 'manifest.json').write_text(json.dumps(entries, indent=2), encoding='utf-8')
    analyze(entries, output / 'analysis', args.warmup, args.sample_seconds)


def parse_session(text):
    exposure, separator, path = text.partition('=')
    if not separator or not path:
        raise argparse.ArgumentTypeError('Use EXPOSURE_US=/absolute/session/path')
    return dict(requested_us=positive(exposure), session=path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    c = sub.add_parser('capture', help='Sequential native capture; never terminate existing camera processes')
    c.add_argument('--output', type=Path, required=True, help='NEW comparison directory')
    c.add_argument('--build-dir', type=Path, default=ROOT / 'out/x5/hvs-build')
    c.add_argument('--exposures-us', nargs='+', type=positive, default=[500, 1000, 5000])
    c.add_argument('--gain', type=positive, default=1)
    c.add_argument('--dgain', type=positive, default=1)
    c.add_argument('--profile', type=int, choices=range(6), default=1)
    c.add_argument('--seconds', type=positive, default=6)
    a = sub.add_parser('analyze', help='Read existing native recordings; no camera access')
    a.add_argument('--session', action='append', type=parse_session)
    a.add_argument('--manifest', type=Path, help='Manifest from an earlier capture; includes logs/readbacks')
    a.add_argument('--output', type=Path, required=True, help='NEW report directory')
    for p in (c, a):
        p.add_argument('--warmup', type=positive, default=1, help='Skip first second for statistics only; keep all recorded frames')
        p.add_argument('--sample-seconds', type=positive, default=3)
    args = parser.parse_args(argv)
    if args.mode == 'capture':
        if len(args.exposures_us) < 2:
            parser.error('Use at least two exposures')
        capture(args)
    else:
        if bool(args.session) == bool(args.manifest):
            parser.error('Use --session entries OR --manifest')
        entries = json.loads(args.manifest.read_text()) if args.manifest else args.session
        analyze(entries, args.output, args.warmup, args.sample_seconds)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print('ERROR:', error, file=sys.stderr)
        sys.exit(1)

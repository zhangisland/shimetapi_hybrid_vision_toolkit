#!/usr/bin/env python3
"""HVS / RDK-X5: build, record, graceful stop, playback, CSV/NPZ export and diagnosis."""
import argparse
import hashlib
import math
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
DEFAULT_BUILD = ROOT / 'out' / 'x5' / 'hvs-build'

def number(text):
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError('Expected a finite nonnegative number')
    return value

def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)

def executable(build, name, subdir):
    path = build / 'samples' / 'cpp' / subdir / name
    if not path.is_file():
        raise RuntimeError(f'Missing {path}; run build first (player needs OpenCV).')
    return path

def check_record_sources(root):
    required = ['CMakeLists.txt', 'samples/CMakeLists.txt',
                'samples/cpp/hvs_record/CMakeLists.txt', 'samples/cpp/hvs_record/main.cpp',
                'samples/cpp/hvs_record/dual_stream_writer.h',
                'samples/cpp/hvs_record/recording_storage.h',
                'samples/cpp/hvs_record/aps_metadata.h',
                'samples/cpp/hvs_record/avi_timing.h']
    missing = [name for name in required if not (root / name).is_file()]
    if missing:
        raise RuntimeError('Incomplete HVS source deployment: missing ' + ', '.join(missing) +
                           '. Copy the complete updated toolkit sources, not only hvs.py.')
    samples = (root / 'samples/CMakeLists.txt').read_text(encoding='utf-8-sig')
    samples = re.sub(r'#[^\n]*', '', samples)
    if not re.search(r'add_subdirectory\s*\(\s*"?cpp/hvs_record"?\s*\)', samples, re.I):
        raise RuntimeError('samples/CMakeLists.txt has not registered the recorder. Add '
                           'add_subdirectory(cpp/hvs_record), or copy the updated file.')


def check_build_targets(build, targets):
    result = run(['cmake', '--build', build, '--target', 'help'],
                 capture_output=True, text=True)
    available = set(re.findall(r'[A-Za-z_][A-Za-z_0-9]*', result.stdout or ''))
    missing = [target for target in targets if target not in available]
    if missing:
        hint = ('Recorder target missing: check the root/samples CMakeLists.txt registration.'
                if 'hv_hvs_record' in missing else
                'CSV converter target missing: check samples/cpp/hvs_raw_to_csv and its CMake registration.'
                if 'hv_hvs_raw_to_csv' in missing else
                'Player target missing: install OpenCV development libraries and reconfigure, '
                'or build without --with-player for recording only.')
        raise RuntimeError('CMake did not generate: ' + ', '.join(missing) + '. ' + hint)


def runtime_env(vin_bypass=False):
    env = os.environ.copy()
    # Explicitly choose this distribution of X5 libraries, not an old /app/build.
    paths = []
    if vin_bypass:
        from x5_vin_bypass import prepare_library
        override = prepare_library(ROOT)
        paths.append(str(override))
        print('EXPERIMENTAL X5 VIN bypass: ' + str(override), flush=True)
        print('Vendor library unchanged; APS output is grayscale. Hardware validation required.', flush=True)
    paths.append(str(ROOT / 'lib' / 'x5'))
    if env.get('LD_LIBRARY_PATH'):
        paths.append(env['LD_LIBRARY_PATH'])
    env['LD_LIBRARY_PATH'] = ':'.join(paths)
    return env

def print_exposure_summary(summary):
    """Old recordings keep missing values unknown; never substitute requests."""
    for label, keys in (
        ('REQUEST', ('aps_exposure_requested_us', 'aps_gain_requested', 'aps_dgain_requested')),
        ('SUBMIT', ('aps_exposure_submitted_seconds', 'aps_gain_submitted', 'aps_dgain_submitted')),
        ('SDK READBACK (configuration only)', ('aps_exposure_readback_seconds', 'aps_gain_readback', 'aps_dgain_readback')),
        ('SENSOR ACTUAL', ('aps_exposure_actual_us', 'aps_gain_actual', 'aps_dgain_actual')),
    ):
        print('Exposure ' + label + ': ' + ', '.join(f'{key}={summary.get(key, "unknown")}' for key in keys), flush=True)
    print('Gain units:', summary.get('aps_gain_units', 'unknown'),
          '; AE readback:', summary.get('aps_ae_readback', 'unknown'), flush=True)


def read_summary(folder):
    path = folder / 'summary.txt'
    if not path.is_file():
        return None
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build', help='Build on X5 or a Linux cross-compilation host')
    b.add_argument('--build-dir', type=Path, default=DEFAULT_BUILD)
    b.add_argument('--cross', action='store_true')
    b.add_argument('--with-player', action='store_true')
    b.add_argument('--jobs', type=int, default=2)
    b.add_argument('--with-native-live', action='store_true')
    b.add_argument('--platform-samples', type=Path, default=Path('/app/multimedia_samples'))
    b.add_argument('--sdk-root', type=Path, default=Path('/usr/hobot'))
    b.add_argument('--sdk-include-dir', type=Path, action='append', default=[],
                   help='Additional matching SDK header directory; repeat for split layouts')
    live = sub.add_parser('live', help='Native dual VC + APS ISP approximate synchronized display')
    live.add_argument('--build-dir', type=Path, default=DEFAULT_BUILD)
    live.add_argument('live_args', nargs=argparse.REMAINDER, help='Use -- then native options; --help for executable help')
    r = sub.add_parser('record', help='Start foreground recording; Ctrl+C gracefully stops')
    r.add_argument('--build-dir', type=Path, default=DEFAULT_BUILD)
    r.add_argument('--output', type=Path, required=True, help='New session directory; must not exist')
    r.add_argument('--x5-vin-bypass', action='store_true',
                   help='Experimental: bypass ISP using an exact-version guarded library copy')
    r.add_argument('--seconds', type=number, default=0)
    r.add_argument('--timeout', type=number, default=10)
    r.add_argument('--aps-stall-timeout', type=number, default=2,
                   help='Maximum gap after the first APS frame, in seconds')
    r.add_argument('--aps-width', '--width', dest='width', type=int, default=1632)
    r.add_argument('--aps-height', '--height', dest='height', type=int, default=1224)
    r.add_argument('--evs-width', type=int, default=768)
    r.add_argument('--evs-height', type=int, default=608)
    r.add_argument('--storage', choices=['memory', 'disk'], default='memory')
    r.add_argument('--ram-dir', type=Path, default=Path('/dev/shm'))
    r.add_argument('--max-mib', type=number, default=1024)
    s = sub.add_parser('stop', help='Request graceful stop from another terminal')
    s.add_argument('--output', type=Path, required=True)
    s.add_argument('--wait', type=number, default=30)
    p = sub.add_parser('play', help='Replay APS+EVS using the existing SDK player')
    p.add_argument('--build-dir', type=Path, default=DEFAULT_BUILD)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--aps-bayer', choices=['auto', 'none', 'rggb', 'bggr', 'grbg', 'gbrg', 'compare'], default='auto',
                   help='auto: preserved Gray8 uses the existing gbrg preset; native NV12 uses none')
    p.add_argument('--speed', type=number, default=1)
    p.add_argument('--dump-timestamps', action='store_true', help='Inspect without a GUI')
    for kind in ('csv', 'npz'):
        c = sub.add_parser('export-' + kind, help='Decode hvs_record RAW8 to ' + kind.upper())
        c.add_argument('--build-dir', type=Path, default=DEFAULT_BUILD)
        c.add_argument('--input', type=Path, required=True, help='events.raw produced by hvs_record')
        c.add_argument('--output', type=Path, required=True, help='New output file; must not exist')
    p.add_argument('--aps-config', type=Path, help='Software ISP JSON; used only for explicit Bayer playback')
    p.add_argument('--aps-wb', choices=['off','manual','once','continuous'])
    sub.add_parser('aps-capabilities', help='Report verified bundled SDK capabilities; no hardware probe')
    for key in ('exposure-us','fps','format','gain','ae','hardware-wb'):
        r.add_argument('--aps-'+key, help='Unavailable in bundled SDK; explicit error before capture')
    d = sub.add_parser('diagnose', help='Read-only platform/library checks')
    d.add_argument('--build-dir', type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args(argv)
    if args.command == 'aps-capabilities':
        print((ROOT / 'tools/aps_capabilities.json').read_text(encoding='utf-8'))
        return 0
    if args.command == 'record':
        for key in ('exposure_us','fps','format','gain','ae','hardware_wb'):
            if getattr(args, 'aps_'+key) is not None:
                raise RuntimeError('APS '+key+' unavailable in bundled SDK. Run aps-capabilities; no device setting was changed.')
    if args.command == 'stop':
        folder = args.output.resolve()
        summary = read_summary(folder)
        if summary:
            print(summary)
            return 0 if summary.get('status') == 'complete' else 2
        if not folder.is_dir() or not ((folder / 'events.raw').is_file() or
                                       (folder / 'recording.storage').is_file()):
            raise RuntimeError('No recording session at this path')
        (folder / 'stop.request').touch(exist_ok=True)
        end = time.monotonic() + args.wait
        while time.monotonic() < end:
            summary = read_summary(folder)
            if summary:
                print(summary)
                return 0 if summary.get('status') == 'complete' else 2
            time.sleep(0.2)
        raise RuntimeError('Stop requested but completion not confirmed. Inspect recording terminal; do not kill -9.')
    if platform.system() != 'Linux':
        raise RuntimeError('Run this command on Linux / RDK-X5. The supplied .so files cannot run on Windows.')
    build = args.build_dir.resolve()
    if args.command == 'build':
        if args.jobs < 1:
            parser.error('--jobs must be positive')
        if not args.cross and platform.machine().lower() not in ('aarch64', 'arm64'):
            parser.error('On an x86 Linux host use --cross for X5')
        check_record_sources(ROOT)
        csv_dir = ROOT / 'samples/cpp/hvs_raw_to_csv'
        if not all((csv_dir / name).is_file() for name in ('main.cpp', 'CMakeLists.txt', 'export_npz.py')):
            raise RuntimeError('Incomplete CSV converter sources: copy samples/cpp/hvs_raw_to_csv')
        command = ['cmake', '-S', ROOT, '-B', build, '-DHV_TOOLKIT_ARCH=x5', '-DBUILD_SAMPLES=ON']
        if args.cross:
            command.append('-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'toolchains/toolchain-aarch64-linux-gnu.cmake'))
        command += ['-DHV_X5_NATIVE=' + ('ON' if args.with_native_live else 'OFF')]
        if args.with_native_live:
            command += ['-DHV_PLATFORM_SAMPLES=' + str(args.platform_samples.resolve()),
                        '-DHV_X5_SDK_ROOT=' + str(args.sdk_root.resolve()),
                        '-DHV_X5_SDK_INCLUDE_DIRS=' + ';'.join(str(p.resolve()) for p in args.sdk_include_dir)]
        run(command)
        targets = ['hv_hvs_record', 'hv_hvs_raw_to_csv']
        if args.with_native_live:
            targets.append('hv_sample_live_record_display')
        if args.with_player:
            targets.append('hv_sample_player')
        check_build_targets(build, targets)
        run(['cmake', '--build', build, '--parallel', args.jobs, '--target', *targets])
        print('Built. Deploy the toolkit directory with lib/x5 and this build directory to X5.')
        return 0
    if args.command == 'live':
        cache = build / 'CMakeCache.txt'
        if not cache.is_file() or 'HV_X5_NATIVE:BOOL=ON' not in cache.read_text():
            raise RuntimeError('Build first with --with-native-live; native live never uses VIN bypass.')
        binary = executable(build, 'hv_sample_live_record_display', 'live_record_display')
        options = args.live_args[1:] if args.live_args[:1] == ['--'] else args.live_args
        with binary.open('rb') as stream:
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        print(f'Native executable: {binary.resolve()} sha256={digest.hexdigest()}', flush=True)
        print('Native arguments:', options, flush=True)
        os.execve(str(binary), [str(binary), *options], runtime_env())
    if args.command == 'diagnose':
        print('Platform:', platform.platform(), platform.machine())
        print('SDK X5 libraries:', ROOT / 'lib/x5')
        for command in (['uname', '-a'], ['df', '-h', str(ROOT)],
                        ['dpkg-query', '-W', 'hobot*'],
                        ['ldd', str(build / 'samples/cpp/hvs_record/hv_hvs_record')]):
            try:
                subprocess.run(command, env=runtime_env(), check=False)
            except OSError as exc:
                print(exc)
        return 0
    if args.command in ('export-csv', 'export-npz'):
        source, destination = args.input.resolve(), args.output.resolve()
        if not source.is_file() or not source.stat().st_size:
            raise RuntimeError('Missing or empty input events.raw')
        if destination.exists():
            raise RuntimeError('Output exists; choose a NEW output file to prevent overwrite')
        binary = executable(build, 'hv_hvs_raw_to_csv', 'hvs_raw_to_csv')
        if args.command == 'export-npz':
            helper = ROOT / 'samples/cpp/hvs_raw_to_csv/export_npz.py'
            run([sys.executable, helper, '--decoder', binary, '--input', source,
                 '--output', destination], env=runtime_env())
        else:
            run([binary, source, destination], env=runtime_env())
        return 0
    folder = args.output.resolve()
    if args.command == 'record':
        if args.timeout <= 0 or args.aps_stall_timeout <= 0 or not 1 <= args.max_mib <= 2048:
            parser.error('--timeout must be positive; --max-mib must be in [1,2048]')
        if any(n < 2 or n > 8192 or n % 2 for n in (args.width, args.height, args.evs_width, args.evs_height)):
            parser.error('Dimensions must be even integers in [2,8192]')
        if folder.exists():
            raise RuntimeError('Output exists; choose a NEW session directory to prevent overwrite')
        folder.parent.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(folder.parent).free < args.max_mib * 1048576 + 64 * 1048576:
            raise RuntimeError('Insufficient free space for --max-mib plus 64 MiB margin')
        binary = executable(build, 'hv_hvs_record', 'hvs_record')
        command = [str(binary), '--output', str(folder), '--seconds', str(args.seconds),
                   '--timeout', str(args.timeout), '--aps-stall-timeout', str(args.aps_stall_timeout), '--aps-width', str(args.width), '--aps-height', str(args.height),
                   '--evs-width', str(args.evs_width), '--evs-height', str(args.evs_height),
                   '--max-mib', str(args.max_mib), '--storage', args.storage]
        if args.storage == 'memory':
            command += ['--ram-dir', str(args.ram_dir)]
        # Replace this process: Ctrl+C/SIGTERM reaches the C++ signal handler directly.
        os.execve(str(binary), command, runtime_env(vin_bypass=args.x5_vin_bypass))
    if args.command == 'play':
        if args.speed <= 0:
            parser.error('--speed must be positive')
        summary = read_summary(folder)
        if not summary or summary.get('status') != 'complete':
            raise RuntimeError('Recording incomplete or not finalized; inspect summary.txt and capture logs')
        for name in ('events.raw', 'aps.avi'):
            if not (folder / name).is_file() or not (folder / name).stat().st_size:
                raise RuntimeError(f'Missing or empty {name}')
        if not args.dump_timestamps and not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
            raise RuntimeError('No graphical display. Use the X5 desktop/X forwarding, or --dump-timestamps.')
        if args.aps_bayer == 'auto':
            aps_count = int(summary.get('aps_frames', '0'))
            preserved_gray = aps_count > 0 and int(summary.get('gray8_frames', '0')) == aps_count
            # Restore this project's existing gbrg replay preset only when all
            # APS Y planes were preserved Gray8. This is not CFA detection.
            args.aps_bayer = 'gbrg' if preserved_gray else 'none'
            print('APS replay auto: ' + ('preserved Gray8 -> existing gbrg color reconstruction'
                                         if preserved_gray else 'NV12/unknown provenance -> native display'), flush=True)
        if args.aps_bayer != 'none':
            aps_count = int(summary.get('aps_frames', '0'))
            if aps_count <= 0 or int(summary.get('gray8_frames', '0')) != aps_count:
                raise RuntimeError('Bayer replay requires preserved Gray8 APS frames; this session contains NV12 or mixed frames. '
                                   'Replay with --aps-bayer none (scripts/play.sh no longer forces gbrg).')
            print('Bayer reconstruction uses original Y samples; CFA pattern and color calibration must be verified.', flush=True)
        print_exposure_summary(summary)
        player = executable(build, 'hv_sample_player', 'player')
        command = [player, folder / 'events.raw', folder / 'aps.avi', '30', str(args.speed)]
        if args.aps_bayer != 'none':
            command.extend(['--aps-bayer', args.aps_bayer])
        if args.aps_config:
            command.extend(['--aps-config', args.aps_config])
        if args.aps_wb:
            command.extend(['--aps-wb', args.aps_wb])
        if args.dump_timestamps:
            command.append('--dump-timestamps')
        run(command, env=runtime_env())
        return 0
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)

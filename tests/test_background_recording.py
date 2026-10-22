import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import hvs

EXE = os.environ.get('HVS_VIN_FAKE')


class WriterStatus(unittest.TestCase):
    def test_fast_native_route_to_background_and_wait_save_stays_synchronous(self):
        for fmt in ['fast', 'native']:
            with tempfile.TemporaryDirectory() as tmp, \
                 mock.patch.object(hvs.platform, 'system', return_value='Linux'), \
                 mock.patch.object(hvs, 'executable', return_value=Path('/fake/record')), \
                 mock.patch.object(hvs, 'runtime_env', return_value={}), \
                 mock.patch.object(hvs, 'record_in_background', return_value=0) as launch:
                args = ['record','--x5-vin-bypass','--save-format',fmt,'--max-mib','64','--output',str(Path(tmp)/'session')]
                self.assertEqual(hvs.main(args), 0)
                self.assertEqual(launch.call_args.args[0][launch.call_args.args[0].index('--save-format')+1], fmt)
                launch.reset_mock()
                with mock.patch.object(hvs.os, 'execve', side_effect=SystemExit) as execute:
                    with self.assertRaises(SystemExit):
                        hvs.main(args+['--wait-save'])
                    execute.assert_called_once()
                    launch.assert_not_called()

    def test_distinguishes_capture_save_failure_interruption_and_buffered_completion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            job = {'pid': 123, 'start_ticks': '456'}
            (root/'writer.json').write_text(json.dumps(job))
            with mock.patch.object(hvs, 'process_identity', return_value='456'):
                self.assertEqual(hvs.recording_status(root)['state'], 'capturing')
                (root/'capture.done').write_text('status=complete\n')
                self.assertEqual(hvs.recording_status(root)['state'], 'saving')
                (root/'save.failed').write_text('disk full')
                self.assertEqual(hvs.recording_status(root)['state'], 'save_failed_retry_required')
                (root/'save.failed').unlink()
            with mock.patch.object(hvs, 'process_identity', return_value='different-process'):
                self.assertEqual(hvs.recording_status(root)['state'], 'interrupted_no_complete_recording')
            (root/'summary.txt').write_text('status=complete\ndurability=kernel_writeback_pending\n')
            self.assertEqual(hvs.recording_status(root)['state'], 'complete')
            self.assertEqual(hvs.recording_status(root)['durability'], 'kernel_writeback_pending')


@unittest.skipUnless(EXE and sys.platform.startswith('linux'), 'Linux vin_record_fake required')
class BackgroundLifecycle(unittest.TestCase):
    def test_handoff_releases_camera_before_save_failure_and_retry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);session = root/'session'
            command = [EXE, '--output', str(session), '--max-mib', '64', '--warmup', '.1', '--seconds', '.15']
            code = hvs.record_in_background(command, session, {**os.environ, 'HVS_FAKE_SAVE_FAILURE': '1'})
            self.assertEqual(code, 0)
            job = json.loads((session/'writer.json').read_text())
            def cleanup():
                (session/'retry.request').touch()
                writer = next(p for p in hvs._background_writers if p.pid == job['pid'])
                writer.wait(timeout=10)
            try:
                until = time.monotonic()+5
                while not (session/'save.failed').exists() and time.monotonic()<until:
                    time.sleep(.01)
                self.assertEqual(hvs.recording_status(session)['state'], 'save_failed_retry_required')
                self.assertFalse((session/'summary.txt').exists())
                # Another capture must work while the original writer retains its RAM.
                second = root/'second'
                p = subprocess.run([EXE, '--output', str(second), '--max-mib', '64', '--warmup', '.1',
                                    '--seconds', '.15', '--diagnostic', 'receive'], capture_output=True, text=True, timeout=10)
                self.assertEqual(p.returncode, 0, p.stdout+p.stderr)
                (session/'retry.request').touch()
                until = time.monotonic()+10
                while not (session/'summary.txt').exists() and time.monotonic()<until:
                    time.sleep(.01)
                self.assertEqual(hvs.recording_status(session)['state'], 'complete')
            finally:
                cleanup()

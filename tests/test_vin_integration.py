"""Set HVS_VIN_FAKE to the locally compiled vin_record_fake executable."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

EXE = os.environ.get('HVS_VIN_FAKE')


@unittest.skipUnless(EXE, 'build vin_record_fake and set HVS_VIN_FAKE')
class VinIntegration(unittest.TestCase):
    def start(self, root, extra=(), env=None):
        folder = Path(root)/'session'
        log = open(Path(root)/'log.txt', 'w')
        proc = subprocess.Popen([EXE, '--output', str(folder), '--max-mib', '64',
                                 '--warmup', '0.1', '--seconds', '0.12', *extra],
                                stdout=log, stderr=log, env={**os.environ, **(env or {})})
        log.close()
        self.addCleanup(lambda: proc.kill() if proc.poll() is None else None)
        return proc, folder, log

    def summary(self, folder):
        return dict(line.split('=', 1) for line in (folder/'summary.txt').read_text().splitlines() if '=' in line)

    def test_copy_ownership_save_and_real_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, log = self.start(tmp)
            self.assertEqual(p.wait(20), 0, Path(tmp, 'log.txt').read_text())
            summary = self.summary(folder)
            self.assertEqual(summary['status'], 'complete')
            self.assertEqual(summary['writing_during_capture'], '0')
            self.assertEqual(summary['aps_duplicate_ids'], '0')
            self.assertEqual(summary['aps_frame_id_gaps'], '0')
            rows = [json.loads(line) for line in (folder/'vin.frames.jsonl').read_text().splitlines()]
            aps = [r for r in rows if r['stream'] == 'aps']
            self.assertGreater(len(aps), 0)
            self.assertTrue(any(r['stream'] == 'evs' for r in rows))
            with (folder/'aps.vin.bin').open('rb') as raw:
                for row in aps:
                    raw.seek(row['offset'])
                    self.assertEqual(raw.read(2), bytes([row['frame_id'] & 255, 0]))
                    self.assertEqual(row['stride'], 3280)
                    self.assertEqual(row['bytes'], 3280*1224)
            self.assertFalse((folder/'recording.storage').exists())

    def test_capacity_stops_and_saves(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, _ = self.start(tmp, ['--seconds', '10'])
            self.assertEqual(p.wait(20), 0)
            summary = self.summary(folder)
            self.assertEqual(summary['reason'], 'capacity')
            self.assertLessEqual(int(summary['buffer_peak_bytes']), 32*1048576)
            self.assertGreater(int(summary['aps_capacity_rejected'])+int(summary['evs_capacity_rejected']), 0)

    def test_save_failure_retains_and_retries(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, _ = self.start(tmp, env={'HVS_FAKE_SAVE_FAILURE': '1'})
            until = time.monotonic()+15
            while 'SAVE FAILED' not in Path(tmp, 'log.txt').read_text() and time.monotonic()<until:
                time.sleep(0.05)
            self.assertIsNone(p.poll())
            self.assertFalse((folder/'summary.txt').exists())
            (folder/'retry.request').touch()
            self.assertEqual(p.wait(20), 0)
            self.assertEqual(self.summary(folder)['status'], 'complete')

    def test_stop_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, _ = self.start(tmp, ['--seconds', '10', '--max-mib', '128'])
            until = time.monotonic()+5
            while not (folder/'recording.storage').exists() and time.monotonic()<until:
                time.sleep(0.01)
            time.sleep(0.25)
            self.assertFalse((folder/'aps.vin.bin').exists())
            self.assertFalse((folder/'aps.avi').exists())
            (folder/'stop.request').touch()
            self.assertEqual(p.wait(20), 0)
            self.assertEqual(self.summary(folder)['reason'], 'requested')

    def test_receive_does_not_fake_a_recording(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, _ = self.start(tmp, ['--diagnostic', 'receive'])
            self.assertEqual(p.wait(10), 0)
            self.assertTrue((folder/'diagnostic.txt').is_file())
            self.assertFalse((folder/'summary.txt').exists())
            self.assertFalse((folder/'aps.avi').exists())

    def test_sigint_handler_drains_then_saves(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, _ = self.start(tmp, ['--seconds', '10', '--max-mib', '128'], {'HVS_FAKE_SIGINT': '1'})
            self.assertEqual(p.wait(20), 0)
            self.assertEqual(self.summary(folder)['reason'], 'requested')

    def test_invalid_layout_releases_dma(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, _ = self.start(tmp, ['--diagnostic', 'receive'], {'HVS_FAKE_BAD_RAW_STRIDE': '1'})
            self.assertEqual(p.wait(10), 2)
            self.assertIn('Invalid VIN plane layout', (folder/'diagnostic.txt').read_text())

    def test_unwritable_save_path_retains_then_recovers(self):
        with tempfile.TemporaryDirectory() as tmp:
            p, folder, _ = self.start(tmp, ['--warmup', '0.3'])
            until = time.monotonic()+5
            while not (folder/'recording.storage').exists() and time.monotonic()<until:
                time.sleep(0.01)
            blocked = folder/'saving.partial'
            blocked.write_text('not a directory')
            until = time.monotonic()+10
            while 'SAVE FAILED' not in Path(tmp, 'log.txt').read_text() and time.monotonic()<until:
                time.sleep(0.05)
            self.assertIsNone(p.poll())
            self.assertFalse((folder/'summary.txt').exists())
            blocked.unlink()
            (folder/'retry.request').touch()
            self.assertEqual(p.wait(20), 0)
            self.assertEqual(self.summary(folder)['status'], 'complete')

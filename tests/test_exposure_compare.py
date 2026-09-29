import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from tools import exposure_compare as ec


class ExposureCompareTests(unittest.TestCase):
    def test_values_and_empty_frames(self):
        for value in ('0', '-1', 'nan', 'inf'):
            with self.assertRaises(argparse.ArgumentTypeError):
                ec.positive(value)
        with self.assertRaises(ValueError):
            ec.pixel_stats(b'')
        with self.assertRaises(ValueError):
            ec.pixel_stats(b'x')
        self.assertEqual(ec.parse_session('500=/tmp/a')['requested_us'], 500)

    def test_readback_is_not_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'capture.log'
            log.write_text('Exposure REQUEST us=1000 again=1 dgain=1\n')
            self.assertIsNone(ec.read_attributes(log)['readback_us'])
            log.write_text('ISP AE API accepted request\nISP attribute readback (not per-frame exposure): seconds=0.000996169 again=1 dgain=1\n')
            result = ec.read_attributes(log)
            self.assertTrue(result['api_set_accepted'])
            self.assertAlmostEqual(result['readback_us'], 996.169)

    def test_board_sweep_does_not_pass_on_sdk_readback(self):
        rows = [dict(requested_us=e, readback_us=r, readback_again=1,
                     readback_dgain=1, api_set_accepted=True, y_mean_median=y,
                     y_ge235_pct_median=17, y_le20_pct_median=0, y_mean_stdev=0.2)
                for e, r, y in ((500,498.084293,148.635),
                                 (1000,996.168586,140.614),
                                 (5000,4993.61428,148.989))]
        verdict = ec.assess_response(rows)
        self.assertEqual(verdict['status'], 'failed')
        self.assertFalse(verdict['sensor_exposure_verified'])
        for row, y in zip(rows, (40,65,120)):
            row['y_mean_median'] = y
        self.assertEqual(ec.assess_response(rows)['status'], 'response_observed')
        rows[1]['y_ge235_pct_median'] = 99
        self.assertEqual(ec.assess_response(rows)['status'], 'inconclusive')
        rows[1]['y_ge235_pct_median'] = 0
        rows[1]['readback_dgain'] = 2
        self.assertEqual(ec.assess_response(rows)['status'], 'inconclusive')

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg unavailable')
    def test_real_nv12_decode_and_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entries = []
            for exposure, y in ((500, 40), (5000, 120)):
                folder = root / str(exposure)
                folder.mkdir()
                frame = bytes([y]) * (128 * 96) + bytes([128]) * (128 * 96 // 2)
                result = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error',
                    '-f', 'rawvideo', '-pixel_format', 'nv12', '-video_size', '128x96',
                    '-framerate', '10', '-i', 'pipe:0', '-c:v', 'rawvideo',
                    '-pix_fmt', 'nv12', '-vtag', 'NV12', str(folder / 'aps.avi')],
                    input=frame * 30, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                (folder / 'summary.txt').write_text('status=complete\naps_storage_format=ISP_NV12_uncorrected\nqueue_dropped=0\n')
                (folder / 'aps.frames.csv').write_text('index,host_receive_ns\n' + ''.join(f'{i},{1000000000+i*100000000}\n' for i in range(30)))
                entries.append(dict(requested_us=exposure, session=str(folder)))
            report = ec.analyze(entries, root / 'report', 1, 1)
            self.assertFalse(report['actual_sensor_exposure_verified'])
            for row, expected in zip(report['rows'], (40, 120)):
                self.assertAlmostEqual(row['y_mean_median'], expected, delta=1)
                self.assertEqual(row['sample_frames'], 10)
                self.assertIsNone(row['readback_us'])
                self.assertEqual(row['frames'], 30)
            self.assertTrue((root / 'report/comparison.csv').exists())
            self.assertEqual(len(json.loads((root / 'report/comparison.json').read_text())['rows']), 2)
            with self.assertRaises(FileExistsError):
                ec.analyze(entries, root / 'report', 1, 1)


if __name__ == '__main__':
    unittest.main()

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

MODULE = Path(__file__).resolve().parents[1] / 'samples/cpp/live_record_display/FindX5Sdk.cmake'
HEADERS = 'hbn_isp_api.h hbn_api.h hb_camera_interface.h vin_cfg.h isp_cfg.h n2d_cfg.h hb_camera_data_config.h cam_def.h'.split()

@unittest.skipUnless(shutil.which('cmake'), 'cmake unavailable')
class SdkDiscoveryTests(unittest.TestCase):
    def configure(self, layout, missing=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for i, name in enumerate(HEADERS):
                if missing and i == 0:
                    continue
                directory = root / ('custom' if layout == 'split' and i % 2 else 'include/HAL' if layout != 'flat' else 'include')
                directory.mkdir(parents=True, exist_ok=True)
                (directory/name).touch()  # Discovery-only fixture, never compiled.
            script = root / 'check.cmake'
            script.write_text(f'''set(HV_X5_SDK_ROOT "{root.as_posix()}")
set(HV_X5_SDK_INCLUDE_DIRS "{(root/'custom').as_posix()}")
include("{MODULE.as_posix()}")
hv_find_x5_sdk(result)
''')
            return subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True)

    def test_flat_hal_and_split(self):
        for layout in ('flat', 'hal', 'split'):
            with self.subTest(layout=layout):
                result = self.configure(layout)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('X5 SDK hbn_isp_api.h:', result.stdout)

    def test_missing_header_is_actionable(self):
        result = self.configure('hal', missing=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('header not found: hbn_isp_api.h', result.stderr)
        self.assertIn('--sdk-include-dir', result.stderr)

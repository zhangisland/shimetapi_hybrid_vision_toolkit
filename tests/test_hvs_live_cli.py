import tempfile
import unittest
from pathlib import Path
from unittest import mock
import hvs

class LiveCliTests(unittest.TestCase):
    @mock.patch.object(hvs.platform, 'system', return_value='Linux')
    def test_native_forwarding_no_bypass(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            (folder/'CMakeCache.txt').write_text('HV_X5_NATIVE:BOOL=ON')
            with mock.patch.object(hvs, 'executable', return_value=Path('/fake/live')), \
                 mock.patch.object(hvs.os, 'execve', side_effect=SystemExit) as execute:
                with self.assertRaises(SystemExit):
                    hvs.main(['live','--build-dir',str(folder),'--','--aps-exposure-us','2500','--record'])
                command=execute.call_args.args[1]
                self.assertEqual(command,[str(Path('/fake/live')),'--aps-exposure-us','2500','--record'])
                self.assertNotIn('vin-bypass',execute.call_args.args[2]['LD_LIBRARY_PATH'])

    @mock.patch.object(hvs.platform, 'system', return_value='Linux')
    def test_requires_native_build(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError,'with-native-live'):
                hvs.main(['live','--build-dir',tmp])

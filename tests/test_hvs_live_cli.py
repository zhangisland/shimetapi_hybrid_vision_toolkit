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
            binary=folder/'live'
            binary.write_bytes(b'fake executable')
            with mock.patch.object(hvs, 'executable', return_value=binary), \
                 mock.patch.object(hvs.os, 'execve', side_effect=SystemExit) as execute:
                with self.assertRaises(SystemExit):
                    hvs.main(['live','--build-dir',str(folder),'--','--aps-exposure-us','2500','--record'])
                command=execute.call_args.args[1]
                self.assertEqual(command,[str(binary),'--aps-exposure-us','2500','--record'])
                self.assertNotIn('vin-bypass',execute.call_args.args[2]['LD_LIBRARY_PATH'])

    @mock.patch.object(hvs.platform, 'system', return_value='Linux')
    def test_requires_native_build(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError,'with-native-live'):
                hvs.main(['live','--build-dir',tmp])

    def test_old_metadata_remains_unknown(self):
        import io
        with mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            hvs.print_exposure_summary({'aps_exposure_requested_us': '50'})
        self.assertIn('aps_exposure_actual_us=unknown', output.getvalue())
        self.assertIn('aps_exposure_readback_seconds=unknown', output.getvalue())

    @mock.patch.object(hvs.platform, 'system', return_value='Linux')
    def test_raw_preview_same_controls_no_record(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'vin';binary.write_bytes(b'fake')
            with mock.patch.object(hvs,'executable',return_value=binary), mock.patch.object(hvs.os,'execve',side_effect=SystemExit) as execute:
                with self.assertRaises(SystemExit):
                    hvs.main(['live','--x5-vin-bypass','--','--aps-gain-db','0','--i2c-bus','6'])
                self.assertEqual(execute.call_args.args[1],[str(binary),'--preview','--aps-gain-db','0','--i2c-bus','6'])

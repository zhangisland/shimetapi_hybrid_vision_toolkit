"""CLI integration checks; SDK execution and CMake are mocked."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('hvs_csv', Path(__file__).resolve().parents[1] / 'hvs.py')
hvs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hvs)

@mock.patch.object(hvs.platform, 'system', return_value='Linux')
class CsvTests(unittest.TestCase):
    def test_default_build_includes_converter_without_player(self, _):
        with mock.patch.object(hvs.platform, 'machine', return_value='aarch64'), mock.patch.object(hvs, 'run', return_value=mock.Mock(stdout='hv_hvs_record hv_hvs_raw_to_csv')) as run:
            self.assertEqual(0, hvs.main(['build']))
            command = run.call_args.args[0]
            self.assertIn('hv_hvs_raw_to_csv', command)
            self.assertIn('hv_hvs_record', command)
            self.assertNotIn('hv_sample_player', command)

    def test_export_uses_selected_build_and_runtime_libraries(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'events.raw'
            source.write_bytes(b'fixture')
            output = root / 'events.csv'
            binary = root / 'build/samples/cpp/hvs_raw_to_csv/hv_hvs_raw_to_csv'
            binary.parent.mkdir(parents=True)
            binary.touch()
            with mock.patch.object(hvs, 'run') as run:
                self.assertEqual(0, hvs.main(['export-csv', '--build-dir', str(root / 'build'), '--input', str(source), '--output', str(output)]))
                self.assertEqual([binary, source, output], run.call_args.args[0])
                self.assertTrue(run.call_args.kwargs['env']['LD_LIBRARY_PATH'].startswith(str(hvs.ROOT / 'lib/x5')))

    def test_npz_dispatches_helper_with_built_decoder(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'events.raw'
            source.write_bytes(b'fixture')
            output = Path(tmp) / 'events.npz'
            with mock.patch.object(hvs, 'executable', return_value=Path('/fake/decoder')), mock.patch.object(hvs, 'run') as run:
                self.assertEqual(0, hvs.main(['export-npz', '--input', str(source), '--output', str(output)]))
                command = run.call_args.args[0]
                self.assertEqual(hvs.sys.executable, command[0])
                self.assertEqual('export_npz.py', command[1].name)
                self.assertEqual(Path('/fake/decoder'), command[command.index('--decoder') + 1])
                self.assertEqual(source, command[command.index('--input') + 1])
                self.assertEqual(output, command[command.index('--output') + 1])

    def test_export_preserves_existing_output(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'events.raw'
            path.write_bytes(b'original')
            with mock.patch.object(hvs, 'run') as run:
                with self.assertRaisesRegex(RuntimeError, 'Output exists'):
                    hvs.main(['export-csv', '--input', str(path), '--output', str(path)])
                run.assert_not_called()
            self.assertEqual(b'original', path.read_bytes())

    def test_missing_input_does_not_launch_converter(self, _):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(hvs, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'Missing or empty'):
                hvs.main(['export-csv', '--input', str(Path(tmp) / 'absent.raw'), '--output', str(Path(tmp) / 'out.csv')])
            run.assert_not_called()

    def test_converter_failure_propagates(self, _):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'events.raw'
            source.write_bytes(b'fixture')
            with mock.patch.object(hvs, 'executable', return_value=Path('/fake/converter')), mock.patch.object(hvs, 'run', side_effect=subprocess.CalledProcessError(2, 'converter')):
                with self.assertRaises(subprocess.CalledProcessError):
                    hvs.main(['export-csv', '--input', str(source), '--output', str(Path(tmp) / 'out.csv')])

    def test_missing_converter_target_has_correct_diagnostic(self, _):
        with mock.patch.object(hvs, 'run', return_value=mock.Mock(stdout='hv_hvs_record')):
            with self.assertRaisesRegex(RuntimeError, 'CSV converter target missing'):
                hvs.check_build_targets(Path('/build'), ['hv_hvs_record', 'hv_hvs_raw_to_csv'])

if __name__ == '__main__':
    unittest.main()

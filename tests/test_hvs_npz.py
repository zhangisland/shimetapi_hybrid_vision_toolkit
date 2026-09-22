import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile
import numpy as np

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('export_npz', root / 'samples/cpp/hvs_raw_to_csv/export_npz.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class NpzTests(unittest.TestCase):
    def test_roundtrip_preserves_keys_types_and_large_timestamps(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            expected = {'x': np.array([0, 767, 42], dtype='<u2'),
                        'y': np.array([607, 0, 10], dtype='<u2'),
                        'polarity': np.array([0, 1, 1], dtype='u1'),
                        'timestamp': np.array([0, 2**53+1, 2**63-1], dtype='<i8')}
            for key, data in expected.items():
                data.tofile(folder / key)
            output = folder / 'events.npz'
            module.compress_columns(folder, output)
            with np.load(output, allow_pickle=False) as actual:
                self.assertEqual(set(expected), set(actual.files))
                for key in expected:
                    np.testing.assert_array_equal(expected[key], actual[key])
                    self.assertEqual(expected[key].dtype, actual[key].dtype)
            with zipfile.ZipFile(output) as archive:
                self.assertTrue(all(i.compress_type == zipfile.ZIP_DEFLATED for i in archive.infolist()))
            with self.assertRaises(FileExistsError):
                module.compress_columns(folder, output)

    def test_bad_column_length_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name, _, _ in module.COLUMNS:
                (folder / name).write_bytes(b'x')
            with self.assertRaisesRegex(RuntimeError, 'Inconsistent'):
                module.compress_columns(folder, folder / 'bad.npz')
            self.assertFalse((folder / 'bad.npz').exists())

    def test_compression_failure_removes_partial_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name, _, size in module.COLUMNS:
                (folder / name).write_bytes(bytes(size))
            output = folder / 'events.npz'
            with mock.patch.object(module.zipfile.ZipFile, 'open', side_effect=OSError('disk full')):
                with self.assertRaisesRegex(OSError, 'disk full'):
                    module.compress_columns(folder, output)
            self.assertFalse(output.exists())

    def test_decoder_failure_cleans_temporary_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            source = folder / 'events.raw'
            source.write_bytes(b'raw')
            with mock.patch.object(module.subprocess, 'run', side_effect=RuntimeError('decode failed')):
                with self.assertRaisesRegex(RuntimeError, 'decode failed'):
                    module.export(Path('decoder'), source, folder / 'events.npz')
            self.assertEqual([source], list(folder.iterdir()))

if __name__ == '__main__':
    unittest.main()

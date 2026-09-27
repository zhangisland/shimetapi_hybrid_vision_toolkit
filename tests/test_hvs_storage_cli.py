import tempfile
import unittest
from pathlib import Path
from unittest import mock
import hvs

class StorageCliTests(unittest.TestCase):
    @mock.patch.object(hvs.platform, 'system', return_value='Linux')
    def test_memory_and_disk_forwarding(self, _):
        for mode in ('memory', 'disk'):
            with tempfile.TemporaryDirectory() as tmp, \
                 mock.patch.object(hvs, 'executable', return_value=Path('/fake/record')), \
                 mock.patch.object(hvs.os, 'execve', side_effect=SystemExit) as execute:
                with self.assertRaises(SystemExit):
                    hvs.main(['record', '--output', str(Path(tmp)/'session'), '--storage', mode, '--max-mib', '1'])
                command=execute.call_args.args[1]
                self.assertEqual(mode, command[command.index('--storage')+1])
                self.assertEqual(mode=='memory', '--ram-dir' in command)

    def test_stop_accepts_ram_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            (folder/'recording.storage').write_text('/dev/shm/hvs-test')
            with self.assertRaisesRegex(RuntimeError, 'completion not confirmed'):
                hvs.main(['stop','--output',str(folder),'--wait','0'])
            self.assertTrue((folder/'stop.request').exists())

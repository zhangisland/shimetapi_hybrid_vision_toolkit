import tempfile
import unittest
from pathlib import Path
from unittest import mock
import hvs

class ArchiveCliTests(unittest.TestCase):
    def test_direct_play_without_four_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'summary.txt').write_text('status=complete\nsession_format=vin_zstd_v1\naps_frames=2\ngray8_frames=0\n')
            for name in ['aps.vin.zst','evs.vin.zst']:(root/name).write_bytes(b'archive')
            with mock.patch.object(hvs.platform,'system',return_value='Linux'), mock.patch.object(hvs,'executable',return_value=Path('/fake/player')),mock.patch.object(hvs,'run') as run:
                hvs.main(['play','--output',tmp,'--dump-timestamps'])
                cmd=run.call_args.args[0]
                self.assertEqual(Path(cmd[1]).name,'evs.vin.zst')
                self.assertEqual(Path(cmd[2]).name,'aps.vin.zst')
                self.assertEqual(cmd[cmd.index('--aps-bayer')+1],'gbrg')

    def test_export_routes_without_opening_camera(self):
        with mock.patch.object(hvs.platform,'system',return_value='Linux'), mock.patch.object(hvs,'executable',return_value=Path('/fake/recorder')),mock.patch.object(hvs,'run') as run:
            hvs.main(['export-recording','--input','archive','--output','exported','--max-mib','1024'])
            cmd=run.call_args.args[0]
            self.assertIn('--export-session',cmd)
            self.assertNotIn('--aps-gain-db',cmd)

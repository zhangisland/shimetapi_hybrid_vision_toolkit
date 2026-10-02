import tempfile
import unittest
from pathlib import Path
from unittest import mock
import hvs

class ReplayTests(unittest.TestCase):
    def replay(self, gray, extra=()):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder/'summary.txt').write_text('status=complete\naps_frames=4\ngray8_frames='+str(gray)+'\n')
            (folder/'events.raw').write_bytes(b'events')
            (folder/'aps.avi').write_bytes(b'aps')
            with mock.patch.object(hvs.platform, 'system', return_value='Linux'), \
                 mock.patch.object(hvs, 'executable', return_value=Path('/fake/player')), \
                 mock.patch.object(hvs, 'run') as run:
                hvs.main(['play','--output',str(folder),'--dump-timestamps',*extra])
                return run.call_args.args[0]

    def test_preserved_gray_restores_color(self):
        command=self.replay(4)
        self.assertEqual('gbrg', command[command.index('--aps-bayer')+1])

    def test_player_viewport_forwarding(self):
        command=self.replay(4,['--window-width','1024','--window-height','600'])
        self.assertEqual('1024',command[command.index('--window-width')+1])
        self.assertEqual('600',command[command.index('--window-height')+1])

    def test_native_nv12_does_not_demosaic(self):
        self.assertNotIn('--aps-bayer', self.replay(0))

    def test_mixed_does_not_demosaic(self):
        self.assertNotIn('--aps-bayer', self.replay(2))

    def test_explicit_gray_display_respected(self):
        self.assertNotIn('--aps-bayer', self.replay(4,['--aps-bayer','none']))

    def test_explicit_pattern_respected(self):
        command=self.replay(4,['--aps-bayer','rggb'])
        self.assertEqual('rggb',command[command.index('--aps-bayer')+1])

    def test_nv12_rejects_explicit_bayer(self):
        with self.assertRaisesRegex(RuntimeError,'preserved Gray8'):
            self.replay(0,['--aps-bayer','gbrg'])

import csv
from pathlib import Path
import struct
import tempfile
import unittest
from tools.check_native_recording import check


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')


class NativeRecordCheckTests(unittest.TestCase):
    def fixture(self, folder, rate):
        stream = bytearray(56)
        stream[:4] = b'vids'
        struct.pack_into('<IIII', stream, 20, 100000, rate, 0, 55)
        data = chunk(b'RIFF', b'AVI ' + chunk(b'LIST', b'hdrl' + chunk(b'LIST', b'strl' + chunk(b'strh', stream))))
        (folder/'aps.avi').write_bytes(data)
        with (folder/'aps.frames.csv').open('w', newline='') as output:
            writer = csv.writer(output)
            writer.writerow(['index', 'host_receive_ns'])
            writer.writerows((i, int(1e9+i*1e9/13.75)) for i in range(55))

    def test_four_second_timeline(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.fixture(folder, 1375000)
            result = check(folder)
            self.assertAlmostEqual(result['avi_duration_seconds'], 4)
            self.assertEqual(result['frames'], 55)

    def test_reject_nominal_30fps(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.fixture(folder, 3000000)
            with self.assertRaisesRegex(ValueError, 'differs from host'):
                check(folder)

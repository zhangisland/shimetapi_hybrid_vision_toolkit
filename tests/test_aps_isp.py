import importlib.util
import json
import struct
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import numpy as np
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import hvs
from tools import aps_isp_tuner as isp

class ApsTests(unittest.TestCase):
    def test_unsupported_controls_never_launch_device(self):
        for key in ('exposure-us','fps','format','gain','ae','hardware-wb'):
            with mock.patch.object(hvs.os,'execve') as launch:
                with self.assertRaisesRegex(RuntimeError,'unavailable'):
                    hvs.main(['record','--output','unused','--aps-'+key,'1'])
                launch.assert_not_called()

    def test_uniform_dark_saturated_and_monochrome_abstain(self):
        cfg={**isp.DEFAULT_CONFIG,'wb_gains':[1.2,1,1.1]}
        for v in (0,.2,1):
            np.testing.assert_array_equal(isp.compute_wb_gains(np.full((32,32),v),cfg,'gbrg'),cfg['wb_gains'])
        raw=np.full((32,32),.1); raw[1::2,0::2]=.9
        np.testing.assert_array_equal(isp.compute_wb_gains(raw,cfg,'gbrg'),cfg['wb_gains'])

    def test_four_patterns_and_input_immutable(self):
        cfg={**isp.DEFAULT_CONFIG,'wb_mode':'off','gamma':1}
        for pattern in isp.BAYER_MASK:
            raw=np.array([80,120,160],dtype=np.uint8)[isp.bayer_channel_map(pattern,32,32)]
            copy=raw.copy(); out=isp.run_isp(raw,cfg,pattern)
            np.testing.assert_allclose(out[16,16],[160,120,80],atol=1)
            np.testing.assert_array_equal(raw,copy)

    def test_no_clipping_before_ccm(self):
        cfg={**isp.DEFAULT_CONFIG,'wb_mode':'manual','wb_gains':[4,1,1], 'gamma':1, 'ccm':[[.25,0,0],[0,1,0],[0,0,1]]}
        raw=np.array([100,100,100],dtype=np.uint8)[isp.bayer_channel_map('rggb',32,32)]
        np.testing.assert_allclose(isp.run_isp(raw,cfg,'rggb')[16,16],[100,100,100],atol=1)

    def test_invalid_config(self):
        for key,value in [('black_level',255),('gamma',0),('wb_gains',[float('nan'),1,1]),('wb_gain_max',float('inf'))]:
            with self.assertRaises(ValueError): isp.run_isp(np.zeros((16,16),np.uint8),{key:value},'gbrg')

    def test_missing_named_config_fails(self):
        a=mock.Mock(config='does-not-exist.json')
        with self.assertRaises(FileNotFoundError): isp.cmd_process(a)

    def test_bundled_library_audit_versions(self):
        root=Path(__file__).resolve().parents[1]
        caps=json.loads((root/'tools/aps_capabilities.json').read_text())
        for arch,digest in caps['audited_sha256'].items():
            self.assertEqual(digest,hashlib.sha256((root/'lib'/arch/'libshimetapi_hv.so.2.0.0').read_bytes()).hexdigest())

    def test_exact_y_extraction_and_format_rejection(self):
        def chunk(tag,payload):
            return tag+struct.pack('<I',len(payload))+payload+(b'\0' if len(payload)%2 else b'')
        def avi(compression=b'NV12',uv=128):
            header=struct.pack('<IiiHH4sIiiII',40,4,4,1,12,compression,24,0,0,0,0)
            hdrl=chunk(b'LIST',b'hdrl'+chunk(b'LIST',b'strl'+chunk(b'strf',header)))
            movie=chunk(b'LIST',b'movi'+chunk(b'00db',bytes(range(16))+bytes([uv])*8))
            return chunk(b'RIFF',b'AVI '+hdrl+movie)
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'in.avi'; output=Path(tmp)/'out.raw'
            source.write_bytes(avi())
            isp.extract_y_plane_python(str(source),0,4,4,str(output))
            self.assertEqual(bytes(range(16)),output.read_bytes())
            with self.assertRaises(FileExistsError): isp.extract_y_plane_python(str(source),0,4,4,str(output))
            for data in (avi(b'MJPG'),avi(uv=127),avi()[:-1]):
                source.write_bytes(data)
                with self.assertRaises(ValueError): isp.extract_y_plane_python(str(source),0,4,4,str(Path(tmp)/'bad.raw'))

    def test_extract_requires_provenance(self):
        with self.assertRaisesRegex(ValueError,'preserved-bayer'):
            isp.cmd_extract(mock.Mock(preserved_bayer=False))

if __name__=='__main__': unittest.main()

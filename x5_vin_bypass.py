"""Experimental, version-locked X5 VIN bypass. Never changes the vendor library.

The checked binary's startPipe(X5Pipe&, bool) already creates VIN and DDR buffers.
At VA/file offset 0xf798 it skips ISP only for the EVS pipe. Replace that single
branch in a separate copy so the APS pipe also skips ISP creation. readImageFrame
then selects its existing VIN/Gray8 path because the ISP handle remains zero.
This is a local diagnostic workaround, not a vendor-supported SDK update.
"""
import hashlib
import os
from pathlib import Path
import tempfile

ORIGINAL_SHA256 = '6ae0ae6ba5335f8a2f09b546ab49ccade1e22474ddd5e161a9bdb5f78b147f4f'
PATCHED_SHA256 = '3fba9bf11934a3b0fea2abd4aca8b1f2f0ff5647101468d4a0e4c1a753b3580d'
OFFSET = 0xf798
ORIGINAL = bytes.fromhex('d6040034')  # cbz w22, 0xf830
REPLACEMENT = bytes.fromhex('26000014')  # b 0xf830


def patch_bytes(data):
    actual = hashlib.sha256(data).hexdigest()
    if actual != ORIGINAL_SHA256:
        raise RuntimeError('VIN bypass refuses this SDK version: expected SHA256 '
                           + ORIGINAL_SHA256 + ', got ' + actual)
    if data[OFFSET:OFFSET + 4] != ORIGINAL:
        raise RuntimeError('VIN bypass instruction guard failed')
    patched = data[:OFFSET] + REPLACEMENT + data[OFFSET + 4:]
    if hashlib.sha256(patched).hexdigest() != PATCHED_SHA256:
        raise RuntimeError('VIN bypass output checksum failed')
    return patched


def prepare_library(root):
    root = Path(root)
    source = root / 'lib/x5/libshimetapi_hv.so.2.0.0'
    data = patch_bytes(source.read_bytes())
    directory = root / 'out/x5/vin-bypass' / ORIGINAL_SHA256[:16]
    directory.mkdir(parents=True, exist_ok=True)
    # ELF SONAME is libshimetapi_hv.so.2. Only this opt-in search directory overrides it.
    output = directory / 'libshimetapi_hv.so.2'
    if output.exists():
        if hashlib.sha256(output.read_bytes()).hexdigest() != PATCHED_SHA256:
            raise RuntimeError('Unexpected file in VIN bypass directory: ' + str(output))
        return directory
    temp_name = None
    try:
        with tempfile.NamedTemporaryFile(dir=directory, prefix='prepare-', delete=False) as handle:
            temp_name = handle.name
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temp_name, 0o644)
        os.replace(temp_name, output)
    finally:
        if temp_name and os.path.exists(temp_name):
            os.unlink(temp_name)
    return directory

import tempfile
import unittest
from pathlib import Path
from unittest import mock
import hvs


class VinCli(unittest.TestCase):
    @mock.patch.object(hvs.platform, 'system', return_value='Linux')
    def test_native_routes_and_controls(self, _):
        with tempfile.TemporaryDirectory() as tmp, \
             mock.patch.object(hvs, 'executable', return_value=Path('/fake/record')) as binary, \
             mock.patch.object(hvs, 'runtime_env', return_value={}) as env, \
             mock.patch.object(hvs.os, 'execve', side_effect=SystemExit) as execute:
            with self.assertRaises(SystemExit):
                hvs.main(['record', '--x5-vin-bypass', '--output', str(Path(tmp)/'new'),
                          '--aps-exposure-lines', '100', '--aps-gain-db', '0', '--i2c-bus', '6',
                          '--max-mib', '64', '--diagnostic', 'copy'])
            self.assertEqual(binary.call_args.args[1], 'hv_hvs_record_vin')
            env.assert_called_once_with(vin_bypass=False)
            cmd = execute.call_args.args[1]
            for key, value in [('aps-exposure-lines', '100'), ('aps-gain-db', '0.0'), ('i2c-bus', '6'), ('diagnostic', 'copy')]:
                self.assertEqual(cmd[cmd.index('--'+key)+1], value)

    def test_bad_controls(self):
        for options in [
            ['--aps-exposure-lines', '0', '--i2c-bus', '6'],
            ['--aps-exposure-lines', '1163', '--i2c-bus', '6'],
            ['--aps-gain-db', '25', '--i2c-bus', '6'],
            ['--aps-exposure-us', '1000', '--i2c-bus', '6'],
            ['--aps-exposure-lines', '100'],
            ['--aps-exposure-lines', '100', '--aps-exposure-us', '1000'],
            ['--storage', 'disk'],
            ['--legacy-sdk-bypass'],
        ]:
            with self.subTest(options=options), self.assertRaises(SystemExit):
                hvs.main(['record', '--x5-vin-bypass', '--output', 'not-created', *options])

    def test_sdk_manual_fails_before_device(self):
        with self.assertRaisesRegex(RuntimeError, 'native VIN'):
            hvs.main(['record', '--output', 'not-created', '--aps-gain-db', '0'])

#!/usr/bin/env python3
"""Check that the native driver selector keeps UMI out of other device builds."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('hakios_config', root / 'tools/hakios-config.py')
cfg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cfg)
assert not cfg.DRIVERS_ERROR, cfg.DRIVERS_ERROR
assert not cfg.drivers_built({})['HAKIOS_UMI']
with tempfile.TemporaryDirectory() as tmp:
    # The upstream manifest names redwood as a repository, so a clean core
    # checkout may not have its ignored device directory.  qemu is always
    # present and exercises the same negative selector assertion in that case.
    negative = 'redwood' if (root / 'devices/redwood').is_dir() else 'qemu'
    for device, expected in [('umi', True), (negative, False)]:
        output = Path(tmp) / (device + '.config')
        subprocess.run(['python3', str(root / 'tools/hakios-config.py'),
                        'defconfig', device, '-o', str(output)], check=True)
        assert cfg.drivers_built(cfg.load(str(output)))['HAKIOS_UMI'] == expected
    drivers = [d for d in cfg.DRIVERS if d.name == 'UMI']
    assert len(drivers) == 1
    assert len(drivers[0].files) == 18  # + hakios.usbmux.cpp, hakios.umi_baseband.cpp, hakios.bootlog.c (v29i)
    for target, _ in drivers[0].files:
        source = root / drivers[0].source(target)
        assert source.is_file(), source
subprocess.run(['python3', str(root / 'devices/xiaomi-mi-10/prepare.py'), 'verify'], check=True)
print('PASS: UMI native drivers opt in only, wrappers present, upstream templates pristine')

#!/usr/bin/env python3
"""Offline registry/list tests; optional real ioreg plist captured read-only."""
import argparse
import ctypes as C
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('usb_log', Path(__file__).with_name('usb-log.py'))
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--fixture', type=Path)
a = p.parse_args()
node = {'idVendor': 0x18d1, 'idProduct': 0x1000, 'IORegistryEntryName': 'UMI USB g1',
        'IORegistryEntryChildren': ['scalar', {'IORegistryEntryName': 'interface0'}, 2]}
root = {'IORegistryEntryName': 'Root', 'IORegistryEntryChildren': [node, 'ignored', 7]}
assert u.registry_devices(root)[0]['children'] == ['interface0']
assert u.registry_devices([root]) == u.registry_devices(root)
assert u.registry_devices({'IORegistryEntryChildren': node}) == u.registry_devices(node)
for scalar in ('text', 4, None, b'bytes', True): assert u.registry_devices(scalar) == []
port = {'IORegistryEntryName': 'Port-USB-C', 'AuthorizationRequired': True,
        'AuthorizationPending': True, 'UserAuthorizationPending': True,
        'TransportsUnauthorized': ['USB2']}
assert u.registry_ports([port, 'ignored']) == [port]
if a.fixture:
    actual = plistlib.loads(a.fixture.read_bytes())
    devices = u.registry_devices(actual)
    assert any((d['idVendor'], d['idProduct']) == (0x18d1, 0x4e11) for d in devices)
    assert u.registry_devices([actual]) == devices
    print(f'PASS: real ioreg plist root={type(actual).__name__}, USB devices={len(devices)}, Android found')

with tempfile.TemporaryDirectory(prefix='umi-registry-test-') as directory:
    output = Path(directory)/'registry.jsonl'
    def result(tree): return subprocess.CompletedProcess([], 0, stdout=plistlib.dumps(tree))
    with patch.object(u.sys, 'platform', 'darwin'), patch.object(u.subprocess, 'run', side_effect=[result(root), result([port])]):
        assert len(u.registry_diagnostic(output=output)) == 1
    record = json.loads(output.read_text())
    assert record['ports'][0]['UserAuthorizationPending']
    assert record['kernel_nodes'][0]['idProduct'] == 0x1000
    for bad in (b'bad plist', plistlib.dumps('scalar')):
        with patch.object(u.sys, 'platform', 'darwin'), patch.object(u.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, stdout=bad)):
            assert u.registry_diagnostic(output=output) == []
    with patch.object(u.sys, 'platform', 'darwin'), patch.object(u.subprocess, 'run', side_effect=subprocess.TimeoutExpired('ioreg', 2)):
        assert u.registry_diagnostic(output=output) == []
    assert len(output.read_text().splitlines()) == 4

class Library:
    _name = '/fake/libusb.dylib'
    def __init__(self):
        self.freed = self.exited = False
        self.devices = (C.c_void_p * 2)(1, None)
    def libusb_init(self, ptr): return 0
    def libusb_exit(self, ctx): self.exited = True
    def libusb_get_device_list(self, ctx, ptr):
        C.cast(ptr, C.POINTER(C.POINTER(C.c_void_p)))[0] = C.cast(self.devices, C.POINTER(C.c_void_p))
        return 1
    def libusb_get_device_descriptor(self, d, ptr):
        desc = C.cast(ptr, C.POINTER(u.Descriptor)).contents
        desc.vendor, desc.product, desc.configurations = 0x18d1, 0x4e11, 1
        return 0
    def libusb_free_device_list(self, d, unref): self.freed = True
    def libusb_get_bus_number(self, d): return 0
    def libusb_get_device_address(self, d): return 1
    def libusb_get_device_speed(self, d): return 3
    def __getattr__(self, name): raise AssertionError('Forbidden list-mode operation: '+name)
lib = Library()
with patch.object(u, 'registry_diagnostic', return_value=[]): assert u.list_devices(lib) == 0
assert lib.freed and lib.exited
print('PASS: dict/list/scalar traversal, authorization evidence, nonfatal malformed/timeout diagnostics, list mode without application open/claim/transfers')

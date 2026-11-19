#!/usr/bin/env python3
"""Offline libusb mocks: never load libusb or access hardware."""
import ctypes as C
import importlib.util
from pathlib import Path
import struct
import tempfile

spec = importlib.util.spec_from_file_location('usb_log', Path(__file__).with_name('usb-log.py'))
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)
u.self_test()

class Library:
    def __init__(self, serial=None, devices=None, claim_rc=0, version=0x100, configuration=1):
        pid = 0x1000 if serial == 'umi-xnu1' else 0xd00d
        self.devices = devices if devices is not None else [(pid, 0, serial)]
        self.claim_rc = claim_rc
        self.version, self.configuration = version, configuration
        self.config_sets = self.alt_sets = self.inits = self.exits = 0
        self.claims = self.releases = self.closes = self.freed = self.opens = 0
        self.listbuf = (C.c_void_p * (len(self.devices)+1))(*range(1, len(self.devices)+1), None)
    def libusb_get_device_list(self, ctx, ptr):
        C.cast(ptr, C.POINTER(C.POINTER(C.c_void_p)))[0] = C.cast(self.listbuf, C.POINTER(C.c_void_p))
        return len(self.devices)
    def libusb_free_device_list(self, ptr, unref): self.freed += 1
    def libusb_open(self, device, handle):
        self.opens += 1
        rc = self.devices[device-1][1]
        if rc == 0: C.cast(handle, C.POINTER(C.c_void_p))[0] = device
        return rc
    def libusb_error_name(self, rc): return {0: b'LIBUSB_SUCCESS', -3: b'LIBUSB_ERROR_ACCESS', -6: b'LIBUSB_ERROR_BUSY'}.get(rc, b'LIBUSB_ERROR_OTHER')
    def libusb_get_device_address(self, d): return d
    def libusb_get_device_speed(self, d): return 3
    def libusb_get_configuration(self, h, ptr):
        C.cast(ptr, C.POINTER(C.c_int))[0] = self.configuration
        return 0
    def libusb_set_configuration(self, h, value):
        self.configuration = value; self.config_sets += 1
        return 0
    def libusb_get_device_descriptor(self, d, ptr):
        v = C.cast(ptr, C.POINTER(u.Descriptor)).contents
        v.vendor, v.product, v.serial = 0x18d1, self.devices[d-1][0], 7
        v.configurations, v.usb, v.version = 1, 0x200, self.version
        return 0
    def libusb_get_port_numbers(self, d, ports, n):
        ports[0], ports[1] = 3, 2
        return 2
    def libusb_get_bus_number(self, d): return 1
    def libusb_claim_interface(self, h, interface):
        self.claims += 1
        return self.claim_rc
    def libusb_release_interface(self, h, interface): self.releases += 1
    def libusb_close(self, h): self.closes += 1
    def libusb_control_transfer(self, h, kind, req, value, index, buf, size, timeout):
        if kind == 1 and req == 11:
            assert (value, index, size) == (0, 0, 0)
            self.alt_sets += 1
            return 0
        assert value == 0x307  # cached descriptor's actual string index
        serial = self.devices[h.value-1][2]
        if serial is None: return -7
        data = serial.encode('utf-16-le')
        data = bytes([len(data) + 2, 3]) + data
        C.memmove(buf, data, len(data))
        return len(data)
    def libusb_bulk_transfer(self, h, ep, buf, size, done, timeout):
        data = b'OKAYxnuloader' if ep == 0x81 else buf.raw[:size]
        if ep == 0x81: C.memmove(buf, data, len(data))
        C.cast(done, C.POINTER(C.c_int)).contents.value = len(data)
        return 0
    def libusb_init(self, ctx):
        self.inits += 1
        return 0
    def libusb_exit(self, ctx): self.exits += 1
    def libusb_handle_events_timeout_completed(self, *args): return 0

lib = Library()
d = u.Device(lib, None, 0xd00d, 'xnuloader')
assert lib.claims == 1 and lib.closes == 0 and d.port == (1, (3, 2))
d.close()
assert lib.closes == lib.releases == 1
for serial in ('xnuloader', 'wrong-serial'):
    lib = Library(serial)
    try:
        d = u.Device(lib, None, 0xd00d, 'xnuloader')
        assert serial == 'xnuloader'
        d.close()
    except u.UsbError:
        assert serial == 'wrong-serial'
    assert lib.closes == 1 and lib.releases == (serial == 'xnuloader')
# A SuperSpeed loader and HS gadget can land on different companion buses.
d = u.Device(Library('umi-xnu1'), None, 0x1000, 'umi-xnu1', (2, (1,)))
d.close()

# Exact open errors and absence are distinguishable; all candidates tried.
lib = Library(devices=[(0x1000, -3, 'umi-xnu1')])
try:
    u.Device(lib, None, 0x1000, 'umi-xnu1')
    raise AssertionError('failed open accepted')
except u.UsbError as exc:
    assert 'matches=1' in str(exc) and 'LIBUSB_ERROR_ACCESS' in str(exc) and 'rc=-3' in str(exc)
assert lib.freed == 1 and lib.closes == 0
lib = Library(devices=[])
try:
    u.Device(lib, None, 0x1000, 'umi-xnu1')
    raise AssertionError('absent target accepted')
except u.UsbError as exc:
    assert 'matches=0' in str(exc) and 'open not attempted' in str(exc)
assert lib.opens == 0 and lib.freed == 1
lib = Library(devices=[(0x1000, -3, 'umi-xnu1'), (0x1000, 0, 'wrong'), (0x1000, 0, 'umi-xnu1')])
d = u.Device(lib, None, 0x1000, 'umi-xnu1')
assert lib.opens == 3 and lib.closes == 1 and lib.freed == 1
d.close()
assert lib.closes == 2 and lib.releases == 1
lib = Library('umi-xnu1', claim_rc=-6)
d = u.Device(lib, None, 0x1000, 'umi-xnu1')
assert not d.claimed
# Device-recipient EP0 diagnostics don't require claiming interface 0.
d.close()
assert lib.releases == 0 and lib.closes == 1

# New bulk devices are configured only if needed, then reset interface framing.
for configuration in (0, 1):
    lib = Library('umi-xnu1', version=0x101, configuration=configuration)
    d = u.Device(lib, None, 0x1000, 'umi-xnu1')
    assert d.bulk_log and lib.alt_sets == 1 and lib.config_sets == (configuration == 0)
    d.close()
lib = Library(devices=[(0x1000, 0, 'wrong')], version=0x101, configuration=0)
try:
    u.Device(lib, None, 0x1000, 'umi-xnu1')
    raise AssertionError('mismatched serial accepted')
except u.UsbError: pass
assert lib.config_sets == lib.alt_sets == lib.claims == 0
class BulkDevice:
    bulk_log = True
    def __init__(self, data, short=False): self.data, self.at, self.short = data, 0, short
    def bulk(self, ep, value):
        if ep == 1:
            assert value == b'LOG1'
            self.at = -1
            return b'LOG1'
        if self.at == -1:
            self.at = 0
            return struct.pack('<6I', 0x31474f4c, 1, len(self.data), 4096, 53, 0)
        chunk = self.data[self.at:self.at+value]; self.at += value
        return chunk[:-1] if self.short else chunk
for size in (0, 512, 4096, 5000, 1280 * 1024):
    data = b'b' * size
    assert u.read_kernel(BulkDevice(data)) == (data, 53)
try:
    u.read_kernel(BulkDevice(b'b' * 5000, short=True))
    raise AssertionError('short bulk data accepted')
except u.UsbError: pass

class Clock:
    now = 0
    def time(self): return self.now
    def sleep(self, seconds): self.now += seconds
clock = Clock()
opens = []
partial = b'x' * 1024
class FakeDevice:
    port = (1, (3, 2))
    def __init__(self, lib, ctx, pid, serial, port):
        opens.append((clock.now, pid))
        assert port in (None, self.port)
        self.kind = 'kernel' if pid == 0x1000 else 'loader'
        if not (self.kind == 'loader' and clock.now < .3 or
                self.kind == 'kernel' and .7 <= clock.now < 1.4):
            raise u.UsbError('not present')
        self.responses = None
    def close(self): pass
    def bulk(self, ep, data):
        if clock.now >= .3: raise u.UsbError('bulk: rc=-4')
        if ep == 1 and data == b'oem log':
            self.responses = iter((b'OKAY', b'DATA00000004', b'boot', b'OKAY'))
        return next(self.responses) if ep == 0x81 else data
    def control(self, req, off, size):
        if clock.now >= 1.4: raise u.UsbError('control: rc/length=-4')
        if req == 0x5a:
            length = 2048 if clock.now >= 1 else 4
            return struct.pack('<6I', 0x31474f4c, 1, length, 1024, 1, 0)
        if clock.now >= 1:
            if off == 0: return partial
            raise u.UsbError('control: rc/length=-7')
        return b'boot'
with tempfile.TemporaryDirectory(prefix='umi-usb-log-') as temp:
    path = Path(temp) / 'log'
    lib = Library()
    assert u.follow(lib, 5, path, .25, clock=clock.time, sleep=clock.sleep, factory=FakeDevice) == 0
    assert lib.inits >= 2 and lib.inits == lib.exits
    assert clock.now >= 5 and opens[-1][0] > 2.8
    assert path.read_bytes() == b'boot'
    assert Path(str(path) + '.partial').read_bytes() == partial
    assert not Path(str(path) + '.partial.next').exists()
    assert len([t for t, p in opens if p == 0xd00d and t < .3]) == 1
print('PASS: explicit enumeration/open errors, multiple candidates, EP0 without claim, serial validation, retained claim, SS/HS transition, brief window, partial preservation, full deadline')

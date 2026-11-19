#!/usr/bin/env python3
"""Supervisor-only live logs: loader fastboot snapshots -> UMI bulk/EP0 snapshots.

--check and --self-test are host-only. --list performs read-only device discovery.
Other invocations collect USB logs (supervisor only).
No reboot, upload, partition or arbitrary memory commands are implemented.
Requires the host libusb-1.0 library, no Python USB package.
"""
import argparse
import ctypes as C
import ctypes.util
from pathlib import Path
import struct
import json
import plistlib
import subprocess
import sys
import time
from datetime import datetime


class Descriptor(C.Structure):
    _layout_ = 'ms'
    _pack_ = 1
    _fields_ = [('length', C.c_uint8), ('type', C.c_uint8), ('usb', C.c_uint16),
                ('devclass', C.c_uint8), ('subclass', C.c_uint8), ('protocol', C.c_uint8),
                ('packet', C.c_uint8), ('vendor', C.c_uint16), ('product', C.c_uint16),
                ('version', C.c_uint16), ('manufacturer', C.c_uint8), ('name', C.c_uint8),
                ('serial', C.c_uint8), ('configurations', C.c_uint8)]


class Timeval(C.Structure):
    _fields_ = [('sec', C.c_long), ('usec', C.c_long)]


def report(message):
    print(f'{datetime.now().isoformat(timespec="milliseconds")} usb-log: {message}', flush=True)

MAX_LOG = 4 * 1024 * 1024


def load_library():
    name = ctypes.util.find_library('usb-1.0')
    if not name:
        for p in ('/opt/homebrew/lib/libusb-1.0.dylib', '/usr/local/lib/libusb-1.0.dylib'):
            if Path(p).is_file():
                name = p
                break
    if not name:
        raise RuntimeError('libusb-1.0 is missing (Homebrew: brew install libusb)')
    lib = C.CDLL(name)
    signatures = {
        'init': (C.c_int, [C.POINTER(C.c_void_p)]),
        'exit': (None, [C.c_void_p]),
        'get_device_list': (C.c_ssize_t, [C.c_void_p, C.POINTER(C.POINTER(C.c_void_p))]),
        'free_device_list': (None, [C.POINTER(C.c_void_p), C.c_int]),
        'open': (C.c_int, [C.c_void_p, C.POINTER(C.c_void_p)]),
        'error_name': (C.c_char_p, [C.c_int]),
        'get_device_address': (C.c_uint8, [C.c_void_p]),
        'get_device_speed': (C.c_int, [C.c_void_p]),
        'get_configuration': (C.c_int, [C.c_void_p, C.POINTER(C.c_int)]),
        'set_configuration': (C.c_int, [C.c_void_p, C.c_int]),
        'close': (None, [C.c_void_p]),
        'get_device': (C.c_void_p, [C.c_void_p]),
        'get_device_descriptor': (C.c_int, [C.c_void_p, C.POINTER(Descriptor)]),
        'get_bus_number': (C.c_uint8, [C.c_void_p]),
        'get_port_numbers': (C.c_int, [C.c_void_p, C.c_void_p, C.c_int]),
        'handle_events_timeout_completed': (C.c_int, [C.c_void_p, C.POINTER(Timeval), C.POINTER(C.c_int)]),
        'claim_interface': (C.c_int, [C.c_void_p, C.c_int]),
        'release_interface': (C.c_int, [C.c_void_p, C.c_int]),
        'get_string_descriptor_ascii': (C.c_int, [C.c_void_p, C.c_uint8, C.c_void_p, C.c_int]),
        'control_transfer': (C.c_int, [C.c_void_p, C.c_uint8, C.c_uint8, C.c_uint16, C.c_uint16,
                                     C.c_void_p, C.c_uint16, C.c_uint]),
        'bulk_transfer': (C.c_int, [C.c_void_p, C.c_ubyte, C.c_void_p, C.c_int,
                                  C.POINTER(C.c_int), C.c_uint]),
    }
    for suffix, (result, args) in signatures.items():
        fn = getattr(lib, 'libusb_' + suffix)
        fn.restype, fn.argtypes = result, args
    return lib


class UsbError(RuntimeError):
    pass


def error_text(lib, rc):
    name = lib.libusb_error_name(rc)
    return f'{rc} ({name.decode() if name else "unknown"})'


# The kernel with usb_ncm=1 adds Apple's usbmux interface and presents 05ac:12c0
# with the lockdown UDID as serial (drivers/umi/hakios.usb.c); interface 0 is unchanged.
VENDOR = {0x1000: 0x18d1, 0xd00d: 0x18d1, 0x12c0: 0x05ac}
UDID = '27da00a11185304aa7aebac7de8154212fb67796'


class Device:
    def __init__(self, lib, ctx, pid, serial, expected_port=None):
        self.lib, self.handle, self.claimed = lib, None, False
        self.kind = 'kernel' if pid in (0x1000, 0x12c0) else 'loader'
        self.port = None
        self.bulk_log = False
        devices = C.POINTER(C.c_void_p)()
        count = lib.libusb_get_device_list(ctx, C.byref(devices))
        if count < 0: raise UsbError(f'get_device_list rc={error_text(lib, count)}')
        errors, matches = [], 0
        try:
            for i in range(count):
                device = devices[i]
                desc = Descriptor()
                rc = lib.libusb_get_device_descriptor(device, C.byref(desc))
                if rc:
                    errors.append(f'descriptor[{i}] rc={error_text(lib, rc)}')
                    continue
                if (desc.vendor, desc.product) != (VENDOR[pid], pid): continue
                matches += 1
                ports = (C.c_uint8 * 8)()
                n = lib.libusb_get_port_numbers(device, ports, len(ports))
                bus = lib.libusb_get_bus_number(device)
                address = lib.libusb_get_device_address(device)
                self.port = (bus, tuple(ports[:n])) if n > 0 else None
                ident = f'{VENDOR[pid]:04x}:{pid:04x} bus={bus} address={address} port={self.port}'
                handle = C.c_void_p()
                rc = lib.libusb_open(device, C.byref(handle))
                # Log EVERY attempted open, even repeated identical failures.
                report(f'{self.kind}: open {ident} rc={error_text(lib, rc)}')
                if rc:
                    errors.append(f'open {ident} rc={error_text(lib, rc)}')
                    continue
                self.handle = handle
                try:
                    if expected_port is not None and self.port != expected_port:
                        report(f'{self.kind}: USB topology changed {expected_port} -> {self.port}; SS/HS companion buses may differ')
                    config = C.c_int(-1)
                    rc = lib.libusb_get_configuration(handle, C.byref(config))
                    report(f'{self.kind}: bcdUSB={desc.usb:#06x} bcdDevice={desc.version:#06x} configurations={desc.configurations} '
                           f'speed={lib.libusb_get_device_speed(device)} active-config={config.value} rc={error_text(lib, rc)}')
                    if desc.serial:
                        raw = C.create_string_buffer(255)
                        n = lib.libusb_control_transfer(handle, 0x80, 6, 0x300 | desc.serial,
                                                        0x409, raw, 255, 200)
                        if n >= 2 and raw.raw[1] == 3 and 2 <= raw.raw[0] <= n:
                            actual = raw.raw[2:raw.raw[0]].decode('utf-16-le', errors='replace').rstrip('\0')
                            if actual != serial:
                                raise UsbError(f'serial mismatch: expected={serial!r}, got={actual!r}')
                            report(f'{self.kind}: serial={actual!r}')
                        else:
                            report(f'{self.kind}: serial descriptor rc/length={n}; continuing with protocol validation')
                    self.bulk_log = self.kind == 'kernel' and desc.version >= 0x0101
                    # Only touch configuration after matching identity. Darwin
                    # may publish a device while it is still unconfigured.
                    if self.kind == 'kernel' and rc == 0 and config.value == 0:
                        rc = lib.libusb_set_configuration(handle, 1)
                        report(f'kernel: set configuration 1 rc={error_text(lib, rc)}')
                        if rc: raise UsbError(f'set configuration rc={error_text(lib, rc)}')
                    rc = lib.libusb_claim_interface(handle, 0)
                    if rc and (self.kind == 'loader' or self.bulk_log):
                        raise UsbError(f'claim interface 0 rc={error_text(lib, rc)}')
                    self.claimed = rc == 0
                    report(f'{self.kind}: claim interface 0 rc={error_text(lib, rc)}')
                    if rc:
                        # c0/5a/5b target the DEVICE recipient, not an interface.
                        # libusb permits EP0 diagnostics without claiming an
                        # interface. Do not force SET_CONFIGURATION or detach.
                        report('kernel: trying device-recipient EP0 diagnostics without interface claim')
                    if self.bulk_log:
                        # Reset this interface's framing after a previous host
                        # read timeout; the kernel cancels both bulk transfers.
                        rc = lib.libusb_control_transfer(handle, 1, 11, 0, 0, None, 0, 700)
                        report(f'kernel: select alt 0 / restart bulk stream rc={error_text(lib, rc)}')
                        if rc: raise UsbError(f'select alt 0 rc={error_text(lib, rc)}')
                    if self.kind == 'loader':
                        self.bulk(1, b'getvar:product')
                        if self.bulk(0x81, 64) != b'OKAYxnuloader':
                            raise UsbError('loader product protocol mismatch')
                    return
                except UsbError as exc:
                    errors.append(f'{ident}: {exc}')
                    self.close()
                except BaseException:
                    self.close()
                    raise
        finally:
            lib.libusb_free_device_list(devices, 1)
        detail = '; '.join(errors) if errors else 'no matching VID/PID in libusb list (open not attempted)'
        raise UsbError(f'target={VENDOR[pid]:04x}:{pid:04x}, list-count={count}, matches={matches}; {detail}')

    def close(self):
        if self.handle:
            if self.claimed:
                self.lib.libusb_release_interface(self.handle, 0)
            self.lib.libusb_close(self.handle)
            self.handle = None
            self.claimed = False

    def control(self, request, offset, size):
        buf = C.create_string_buffer(size)
        n = self.lib.libusb_control_transfer(self.handle, 0xc0, request, offset & 65535,
                                            offset >> 16, buf, size, 700)
        if n != size:
            raise UsbError(f'control {request:#x} offset={offset}: wanted={size}, rc/length={n}')
        return buf.raw[:n]

    def bulk(self, endpoint, data_or_size):
        size = data_or_size if isinstance(data_or_size, int) else len(data_or_size)
        buf = C.create_string_buffer(size)
        if not isinstance(data_or_size, int):
            buf.raw = data_or_size
        done = C.c_int()
        rc = self.lib.libusb_bulk_transfer(self.handle, endpoint, buf, size, C.byref(done), 700)
        if rc:
            raise UsbError(f'bulk {endpoint:#x}: rc={rc}, bytes={done.value}')
        if endpoint == 1 and done.value != size:
            raise UsbError('short command write')
        return buf.raw[:done.value]


def read_kernel(dev, progress=None):
    bulk = getattr(dev, 'bulk_log', False)
    if bulk:
        dev.bulk(1, b'LOG1')
        raw = dev.bulk(0x81, 24)
    else:
        raw = dev.control(0x5a, 0, 24)
    if len(raw) != 24: raise UsbError('short kernel metadata')
    magic, generation, size, chunk, lo, hi = struct.unpack('<6I', raw)
    if magic != 0x31474f4c or not generation or size > 1280 * 1024 or chunk != (4096 if bulk else 1024):
        raise UsbError('invalid kernel snapshot metadata')
    data = bytearray()
    while len(data) < size:
        wanted = min(chunk, size - len(data))
        piece = dev.bulk(0x81, wanted) if bulk else dev.control(0x5b, len(data), wanted)
        if len(piece) != wanted: raise UsbError('short kernel snapshot chunk')
        data += piece
        if progress:
            progress(piece)
    return bytes(data), lo | (hi << 32)


def read_loader(dev):
    dev.bulk(1, b'oem log')
    if not dev.bulk(0x81, 64).startswith(b'OKAY'):
        raise UsbError('loader refused oem log')
    dev.bulk(1, b'upload')
    reply = dev.bulk(0x81, 64)
    if len(reply) != 12 or reply[:4] != b'DATA':
        raise UsbError('invalid loader upload header')
    try:
        size = int(reply[4:], 16)
    except ValueError as exc:
        raise UsbError('invalid loader log size') from exc
    if size > MAX_LOG:
        raise UsbError('loader log exceeds bound')
    data = bytearray()
    while len(data) < size:
        piece = dev.bulk(0x81, min(65536, size - len(data)))
        if not piece:
            raise UsbError('short loader log')
        data += piece
    if not dev.bulk(0x81, 64).startswith(b'OKAY'):
        raise UsbError('loader log missing trailer')
    return bytes(data)


def delta(previous, current):
    if not previous:
        return current
    if current.startswith(previous):
        return current[len(previous):]
    tail = previous[-4096:]
    pos = current.rfind(tail)
    if pos >= 0:
        return current[pos + len(tail):]
    return b'\n--- USB log discontinuity / ring overlap lost ---\n' + current


def registry_nodes(tree):
    """ioreg -a returns a root dict on this Mac; tolerate lists/scalars too."""
    if isinstance(tree, (list, tuple)):
        for node in tree: yield from registry_nodes(node)
    elif isinstance(tree, dict):
        yield tree
        yield from registry_nodes(tree.get('IORegistryEntryChildren'))


def registry_devices(tree):
    found = []
    for node in registry_nodes(tree):
        if 'idVendor' not in node or 'idProduct' not in node: continue
        keys = ('IORegistryEntryName', 'IOObjectClass', 'IORegistryEntryID',
                'idVendor', 'idProduct', 'bNumConfigurations',
                'kUSBCurrentConfiguration', 'bDeviceClass', 'bcdDevice',
                'USB Serial Number', 'IOCFPlugInTypes', 'IOServiceBusyState',
                'IOServiceState', 'sessionID', 'locationID')
        item = {k: node[k] for k in keys if k in node}
        item['children'] = [n.get('IORegistryEntryName') for n in registry_nodes(node.get('IORegistryEntryChildren'))]
        found.append(item)
    return found


def registry_ports(tree):
    keys = ('IORegistryEntryID', 'IORegistryEntryName', 'AuthorizationPending',
            'AuthorizationRequired', 'UserAuthorizationPending',
            'UserAuthorizationStatusDescription', 'TransportsUnauthorized', 'TransportsActive')
    return [{k: n[k] for k in keys if k in n} for n in registry_nodes(tree)
            if 'AuthorizationRequired' in n or 'AuthorizationPending' in n]


def registry_diagnostic(all_devices=False, output=None):
    """Nonfatal registry inspection. Never authorize or open a device."""
    if sys.platform != 'darwin': return []
    devices = []
    record = {'time': datetime.now().isoformat(timespec='milliseconds')}
    for label, command in (
        ('devices', ['/usr/sbin/ioreg', '-a', '-p', 'IOUSB', '-l']),
        ('ports', ['/usr/sbin/ioreg', '-a', '-p', 'IOService', '-r', '-c', 'IOPort', '-l'])):
        try:
            raw = subprocess.run(command, capture_output=True, timeout=2, check=True).stdout
            tree = plistlib.loads(raw)
            if label == 'devices':
                devices = registry_devices(tree)
                kernel = {(0x18d1, 0x1000), (0x05ac, 0x12c0)}
                record[label] = devices if all_devices else [d for d in devices if (d['idVendor'], d['idProduct']) in kernel]
                # Preserve every target property for later analysis, independent
                # of a supervisor's filtered terminal ioreg capture.
                record['kernel_nodes'] = [{k: v for k, v in n.items() if k != 'IORegistryEntryChildren'}
                    for n in registry_nodes(tree) if (n.get('idVendor'), n.get('idProduct')) in kernel]
            else:
                record[label] = registry_ports(tree)
                for port in record[label]:
                    if port.get('UserAuthorizationPending') or port.get('AuthorizationPending'):
                        report(f'Mac accessory authorization pending: {port}; unlock the Mac and allow the new USB accessory; retrying')
            report(f'registry {label} (read-only): {record[label]}')
        except (OSError, ValueError, TypeError, RecursionError, plistlib.InvalidFileException, subprocess.SubprocessError) as exc:
            record[label + '_error'] = str(exc)
            report(f'registry {label} diagnostic unavailable (collection continues): {exc}')
    if output is not None:
        try:
            with Path(output).open('a') as f: f.write(json.dumps(record, default=str) + '\n')
        except OSError as exc:
            report(f'registry evidence write failed (collection continues): {exc}')
    return devices


def list_devices(lib):
    """Enumeration only: no application open/claim/configuration/transfer calls.

    libusb's Darwin backend internally reads descriptors during discovery.
    """
    report(f'libusb library={lib._name}; resolved={Path(lib._name).resolve()}; descriptor-size={C.sizeof(Descriptor)}')
    ctx = C.c_void_p()
    rc = lib.libusb_init(C.byref(ctx))
    if rc: raise UsbError(f'libusb_init: {error_text(lib, rc)}')
    devices = C.POINTER(C.c_void_p)()
    try:
        count = lib.libusb_get_device_list(ctx, C.byref(devices))
        if count < 0: raise UsbError(f'get_device_list: {error_text(lib, count)}')
        report(f'list-count={count}')
        for i in range(count):
            d = Descriptor()
            rc = lib.libusb_get_device_descriptor(devices[i], C.byref(d))
            if rc:
                report(f'descriptor[{i}]: {error_text(lib, rc)}'); continue
            report(f'list[{i}] {d.vendor:04x}:{d.product:04x} bus={lib.libusb_get_bus_number(devices[i])} '
                   f'address={lib.libusb_get_device_address(devices[i])} bcdDevice={d.version:04x} '
                   f'configs={d.configurations} speed={lib.libusb_get_device_speed(devices[i])}')
    finally:
        if devices: lib.libusb_free_device_list(devices, 1)
        lib.libusb_exit(ctx)
    registry_diagnostic(all_devices=True)
    return 0


def follow(lib, seconds, path, interval, *, clock=time.monotonic, sleep=time.sleep, factory=Device):
    ctx = C.c_void_p()
    rc = lib.libusb_init(C.byref(ctx))
    if rc:
        raise UsbError(f'libusb_init: {rc}')
    ctx_active = True
    deadline = clock() + seconds
    previous, seen_kernel, seen_any, last_state = b'', False, False, None
    dev, bound_port = None, None
    failures = 0
    refreshed = diagnosed = clock()
    path.parent.mkdir(parents=True, exist_ok=True)
    partial_path = Path(str(path) + '.partial')
    try:
        with path.open('wb') as transcript:
            while clock() < deadline:
                # Service Darwin hotplug notifications even when no transfer is
                # active. A stale device list must not hide the replacement PID.
                tv = Timeval(0, 0)
                lib.libusb_handle_events_timeout_completed(ctx, C.byref(tv), None)
                errors = []
                if dev is None:
                    # Full Darwin backend rescan after a handover; retain a live
                    # handle, and never reset/detach an attached device.
                    if clock() - refreshed >= 2:
                        lib.libusb_exit(ctx)
                        ctx_active = False
                        ctx = C.c_void_p()
                        rc = lib.libusb_init(C.byref(ctx))
                        if rc: raise UsbError(f'libusb re-init: {error_text(lib, rc)}')
                        ctx_active = True
                        refreshed = clock()
                        report('discovery: recreated libusb context (no device reset)')
                    if factory is Device and clock() - diagnosed >= 5:
                        registry_diagnostic(output=Path(str(path) + '.registry.jsonl')); diagnosed = clock()
                    for pid, serial, kind in ((0x1000, 'umi-xnu1', 'kernel'), (0xd00d, 'xnuloader', 'loader'),
                                              (0x12c0, UDID, 'kernel')):
                        try:
                            dev = factory(lib, ctx, pid, serial, bound_port)
                            if bound_port is None:
                                bound_port = dev.port
                            break
                        except UsbError as exc:
                            errors.append(f'{kind}: {exc}')
                current, state = None, '; '.join(errors)
                if dev is not None:
                    try:
                        if dev.kind == 'kernel':
                            # Commit chunks immediately. If enumeration lasts
                            # only seconds, a failed snapshot still leaves bytes.
                            # Do not erase a useful incomplete snapshot when a
                            # later attempt fails before its first data chunk.
                            with Path(str(partial_path) + '.next').open('wb') as partial:
                                promoted = False
                                def progress(piece):
                                    nonlocal promoted
                                    partial.write(piece)
                                    partial.flush()
                                    if not promoted:
                                        Path(partial.name).replace(partial_path)
                                        promoted = True
                                try:
                                    current, heartbeat = read_kernel(dev, progress)
                                finally:
                                    if not promoted:
                                        Path(partial.name).unlink(missing_ok=True)
                            seen_kernel = True
                            state = f'kernel log bytes={len(current)}, heartbeat={heartbeat}, port={dev.port}, transport={"bulk" if getattr(dev, "bulk_log", False) else "EP0"}'
                        else:
                            current = read_loader(dev)
                            state = f'loader log bytes={len(current)}, port={dev.port}'
                        failures = 0
                    except UsbError as exc:
                        failures += 1
                        state = '; '.join(errors + [f'{dev.kind}: {exc}; consecutive failures={failures}'])
                        # A second failure or a disconnected device gets a fresh
                        # handle. Never reset/reconfigure the controller here.
                        if failures >= 2 or 'rc=-4' in str(exc) or 'rc/length=-4' in str(exc):
                            dev.close()
                            dev = None
                            failures = 0
                if current is not None:
                    seen_any = True
                    transcript.write(delta(previous, current))
                    transcript.flush()
                    previous = current
                    Path(str(path) + '.live').write_bytes(current)
                if state != last_state or (dev is None and errors):
                    report(state)
                    last_state = state
                sleep(min(interval if current is not None else 0.1, max(0, deadline - clock())))
    finally:
        if dev is not None:
            dev.close()
        if ctx_active: lib.libusb_exit(ctx)
    report(f'done; kernel snapshots={seen_kernel}; any log={seen_any}; {path}')
    report('Android return must be checked by supervisor; no host reboot was sent.')
    return 0 if seen_kernel else 2


def self_test():
    # Fake transports only: exercise framing, limits, chunking and ring overlap.
    sample = bytes(range(256)) * 9
    class FakeKernel:
        def control(self, req, offset, size):
            if req == 0x5a:
                return struct.pack('<6I', 0x31474f4c, 1, len(sample), 1024, 42, 0)
            return sample[offset:offset + size]
    assert read_kernel(FakeKernel()) == (sample, 42)
    class BadKernel(FakeKernel):
        def control(self, req, offset, size):
            return struct.pack('<6I', 0x31474f4c, 1, MAX_LOG, 1024, 0, 0)
    try:
        read_kernel(BadKernel())
        raise AssertionError('oversized snapshot accepted')
    except UsbError:
        pass
    class FakeLoader:
        def __init__(self):
            self.responses = iter((b'OKAY', b'DATA00000900', sample, b'OKAY'))
        def bulk(self, endpoint, value):
            return next(self.responses) if endpoint == 0x81 else value
    assert read_loader(FakeLoader()) == sample
    assert delta(b'a', b'abc') == b'bc'
    old = b'old' + b't' * 4096
    assert delta(old, b'head' + b't' * 4096 + b'new') == b'new'
    assert b'discontinuity' in delta(b'old', b'new')
    print('usb-log: offline transport tests passed (no USB library initialized)')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--list', action='store_true', help='read-only discovery and registry comparison; no application device open/claim')
    p.add_argument('--check', action='store_true', help='load library only; no USB access')
    p.add_argument('--self-test', action='store_true', help='fake transports only; no USB access')
    p.add_argument('--seconds', type=int, default=150)
    p.add_argument('--interval', type=float, default=0.25)
    p.add_argument('--log', type=Path, default=Path('usb-stage1.log'))
    args = p.parse_args()
    if args.self_test:
        self_test()
        return 0
    lib = load_library()
    if args.list: return list_devices(lib)
    if args.check:
        print('usb-log: libusb available; no USB context opened')
        return 0
    if not 1 <= args.seconds <= 3700 or args.interval < 0.1:
        p.error('seconds must be 1..3700 and interval >= 0.1')
    return follow(lib, args.seconds, args.log, args.interval)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as exc:
        sys.exit(f'usb-log: {exc}')

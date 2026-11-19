#!/usr/bin/env python3
"""Offline writer regression checks plus artifact verification. Never calls adb/fastboot."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


w = load('umi_writer', HERE / 'darwin-write-umi.py')


def main():
    with patch.object(w.subprocess, 'run', side_effect=AssertionError('offline selftest invoked subprocess')):
        w.selftest(w.GPT_HEAD)
    # su may use a pty: text is normalized; binary is exclusively adb pull.
    with patch.object(w.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'text\r\n\r\nUMI_OK\r\n', b'')) as mock:
        assert w.Phone('offline-test').text('echo text') == 'text\n'
        call = mock.call_args.args[0]
        assert call[:4] == ['adb', '-s', 'offline-test', 'shell']
        assert '</dev/null' in call[4] and 'exec-out' not in call
    payload = bytes(range(256)) * 64  # includes LF, CR, NUL and non-UTF8
    with tempfile.TemporaryDirectory(prefix='verify-', dir=w.OUT) as tmp:
        path = Path(tmp) / 'pulled'
        phone = w.Phone('offline-test')
        def pulled(args):
            assert args[:2] == ['pull', '/data/local/tmp/offline']
            path.write_bytes(payload)
        phone.run = pulled
        phone.remote_hash = lambda _: w.sha(payload)
        phone.pull_checked('/data/local/tmp/offline', path, len(payload))
        phone.remote_hash = lambda _: w.sha(payload.replace(b'\n', b'\r\n'))
        try:
            phone.pull_checked('/data/local/tmp/offline', path, len(payload))
        except w.Refused:
            pass
        else:
            raise AssertionError('corrupt binary pull accepted')
    manifest = json.loads((w.OUT / 'manifest.json').read_text())
    for name, facts in manifest['files'].items():
        path = w.OUT / name
        assert path.stat().st_size == facts['bytes'], name
        with path.open('rb') as f:
            assert hashlib.file_digest(f, 'sha256').hexdigest() == facts['sha256'], name
    for name, block in (('rootfs.apfs', 0), ('cryptex-os.apfs', 1374208), ('cryptex-app.apfs', 2039808)):
        kind, extents = w.image_plan(w.OUT / name, block)
        assert len(extents) == 1 and kind in ('root', 'os', 'app')
    sys.path.insert(0, str(ROOT / 'tools/apfs'))
    import apfs
    kernel = ROOT / 'out/xiaomi-mi-10/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837'
    flash_payloads = ['root-flash-data-test.apfs', 'root-flash-ui.apfs']
    if (w.OUT / 'root-flash-ui-ca.apfs').exists():
        flash_payloads.append('root-flash-ui-ca.apfs')
    for name in flash_payloads:
        c = apfs.Container(str(w.OUT / name))
        v = apfs.pick_volume(c, None)
        hit = v.lookup('/lib/libOSMesa.8.dylib')
        assert hit is not None
        inode = v.inode(hit[0])
        assert inode.mode & 0o170000 == 0o100000  # real library, not a link into the RAM system
        assert inode.size > 4 * 1024 * 1024
        assert v.lookup('/lib/HakiosMetal.bundle/HakiosMetal') is not None
        if name == 'root-flash-ui-ca.apfs':
            metal = v.inode(v.lookup('/lib/HakiosMetal.bundle/HakiosMetal')[0])
            assert metal.mode & 0o170000 == 0o100000
            assert b'CoreAnimation\'s shaders:' in metal.contents()
            assert b'_air_to_nir' in metal.contents()  # native AIR compiler linked
            assert b'CoreAnimation exact configuration:' in metal.contents()
            # (the exact path compiles iOS 16.5's own library functions: no QuartzCore-exact.plist sidecar any more)
            metallib = v.lookup('/share/ca-share/QuartzCore-default.metallib')
            assert metallib is not None
            assert hashlib.sha256(v.inode(metallib[0]).contents()).hexdigest() == hashlib.sha256(
                (ROOT / 'out/xiaomi-mi-10/ca-share/QuartzCore-default.metallib').read_bytes()).hexdigest()
        assert b'the RAM Metal plug-in on the flash root' in v.inode(v.lookup('/sbin/init')[0]).contents()
        assert kernel.stat().st_size + (w.OUT / name).stat().st_size + 4096 <= 63 * 1024 * 1024
        c.close()
    kind, extents = w.image_plan(w.OUT / 'data.simg', 0)
    assert kind == 'sparse-data'
    assert all(at >= w.DATA_FIRST for at, *_ in extents)
    # Reconstruct only the data volume as a local sparse regular file. Independently check
    # the allocation bitmap: every allocated HFS block must be supplied by the sparse image.
    with tempfile.TemporaryDirectory(prefix='verify-hfs-', dir=w.OUT) as tmp:
        path = Path(tmp) / 'data.hfs'
        with path.open('w+b') as f:
            f.truncate((w.BLOCKS - w.DATA_FIRST) * w.BS)
            for at, data in w.pieces(w.OUT / 'data.simg', extents):
                f.seek((at - w.DATA_FIRST) * w.BS)
                f.write(data)
            mk = load('upstream_mkdatavol', ROOT / 'tools/mkdatavol.py')
            total, used = mk.used_blocks(f)
            assert total == w.BLOCKS - w.DATA_FIRST
            f.seek(1024)
            vh = f.read(512)
            assert struct.unpack_from('>I', vh, 4)[0] & 0x2000  # journaled
            covered = bytearray(total)
            for at, n, *_ in extents:
                covered[at - w.DATA_FIRST:at - w.DATA_FIRST + n] = b'\1' * n
            assert all(not used(b) or covered[b] for b in range(total))
            # A corrupt primary HFS size must fail even with a structurally valid sparse container.
            image = bytearray((w.OUT / 'data.simg').read_bytes())
            for at, count, offset, word in extents:
                if at == w.DATA_FIRST and word is None:
                    image[offset + 1024 + 44] ^= 1
                    break
            else:
                raise AssertionError('no primary HFS header extent')
            bad = Path(tmp) / 'bad.simg'
            bad.write_bytes(image)
            try:
                w.image_plan(bad, 0)
            except w.Refused:
                pass
            else:
                raise AssertionError('corrupt HFS geometry accepted')
    print('PASS: GPT/range refusals, pty text handling, binary pull and corruption checks, '
          'all artifact SHA256s, APFS slots, flash Mesa/Metal paths and payload size, '
          'HFSX headers/journal/allocation coverage')


if __name__ == '__main__':
    main()

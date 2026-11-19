#!/usr/bin/env python3
"""Guarded umi writer. No phone command is run by selftest or --help.

  selftest [GPT_HEAD]             offline, default the supervisor's carved GPT
  --serial SERIAL check          inspect GPT, UFS LUN0 and sda35 identity
  --serial SERIAL plan IMAGE BLOCK
  --serial SERIAL write IMAGE BLOCK   requires DARWIN_WRITE_UMI=yes
  --serial SERIAL sum BLOCK COUNT

4K blocks are relative to darwin. Apple APFS images must exactly match a slot;
data writes must stay at/after DATA_FIRST. Android sparse input is accepted ONLY
for data, at BLOCK=0, with DONT_CARE over all system blocks. Sparse DONT_CARE is
never written or hashed. Binary su output never crosses adb shell: transfers use
push/pull of bounded temporary files, with SHA256 checked on both sides.
"""
import argparse
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile
import time
import uuid

from layout import (BS, DISK_BLOCKS, PARTN, FIRST, LAST, BLOCKS, TYPE, UNIQUE,
                    DEV, NODE, DATA_FIRST, SLOTS)

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'out/xiaomi-mi-10/flash'
GPT_HEAD = Path('/Users/xm666/X/umi-backup/carve-64g/gpt-head.new')
CHUNK = 16 * 1024 * 1024
MAGIC, RAW, FILL, DONT_CARE = 0xed26ff3a, 0xcac1, 0xcac2, 0xcac3


class Refused(Exception):
    pass


def require(ok, message):
    if not ok:
        raise Refused(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def audit(message):
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / 'writer.jsonl').open('a') as f:
        f.write(json.dumps({'time': time.time(), 'message': message}) + '\n')
    print(message, flush=True)


def check_gpt(head):
    """Complete primary head including LBA0. Check CRCs and ALL occupied entries."""
    require(len(head) == 8 * BS, 'GPT head must be exactly eight 4K blocks')
    h = bytearray(head[BS:2 * BS])
    require(h[:8] == b'EFI PART', 'no GPT at LBA1')
    rev, hsize, crc, reserved = struct.unpack_from('<IIII', h, 8)
    require(rev == 0x10000 and hsize == 92 and reserved == 0, 'unexpected GPT header format')
    struct.pack_into('<I', h, 16, 0)
    require(binascii.crc32(h[:hsize]) & 0xffffffff == crc, 'GPT header CRC')
    require(struct.unpack_from('<QQQQ', h, 24) == (1, DISK_BLOCKS - 1, 6, LAST),
            'GPT disk geometry changed')
    elba, n, esize, tcrc = struct.unpack_from('<QIII', h, 72)
    require((elba, n, esize) == (2, 64, 128), 'GPT table geometry changed')
    table = head[elba * BS:elba * BS + n * esize]
    require(binascii.crc32(table) & 0xffffffff == tcrc, 'GPT table CRC')
    entries, darwin = [], []
    for i in range(n):
        e = table[i * esize:(i + 1) * esize]
        if e[:16] == bytes(16):
            continue
        name = e[56:128].decode('utf-16le', errors='strict').rstrip('\0')
        start, end = struct.unpack_from('<QQ', e, 32)
        require(6 <= start <= end <= LAST, f'entry {i} outside usable LBAs')
        entries.append((start, end, i, name))
        if name == 'darwin':
            darwin.append((i + 1, str(uuid.UUID(bytes_le=e[:16])),
                           str(uuid.UUID(bytes_le=e[16:32])), start, end))
    require(darwin == [(PARTN, TYPE, UNIQUE, FIRST, LAST)], 'darwin entry/name/GUID/LBAs changed')
    entries.sort()
    for a, b in zip(entries, entries[1:]):
        require(a[1] < b[0], f'overlapping GPT entries {a[2]} and {b[2]}')
    return True


def bounds(block, count, data=False):
    require(block >= (DATA_FIRST if data else 0) and count > 0 and block + count <= BLOCKS,
            f'range {block}+{count} outside allowed darwin blocks')


def image_plan(path, block):
    """(kind, extents): extent=(darwin block, count, file offset, fill word or None)."""
    size = path.stat().st_size
    with path.open('rb') as f:
        hdr = f.read(4096)
        if hdr[:4] == struct.pack('<I', MAGIC):
            require(block == 0, 'sparse images describe all of darwin: BLOCK must be 0')
            require(len(hdr) >= 28, 'short sparse header')
            magic, major, minor, fhs, chs, bs, total, chunks, crc = struct.unpack_from('<IHHHHIIII', hdr)
            require((major, minor, fhs, chs, bs, total, crc) == (1, 0, 28, 12, BS, BLOCKS, 0),
                    'unexpected sparse geometry/version/checksum')
            require(0 < chunks <= size // 12, 'invalid sparse chunk count')
            f.seek(fhs)
            at, extents = 0, []
            for _ in range(chunks):
                raw = f.read(chs)
                require(len(raw) == chs, 'truncated sparse chunk')
                kind, reserved, count, csize = struct.unpack('<HHII', raw)
                require(reserved == 0 and count > 0 and at + count <= BLOCKS, 'sparse chunk bounds')
                payload = csize - chs
                if kind == DONT_CARE:
                    require(payload == 0, 'DONT_CARE has payload')
                else:
                    bounds(at, count, data=True)
                    require(kind in (RAW, FILL), 'unsupported sparse chunk')
                    require(payload == (count * BS if kind == RAW else 4), 'sparse payload size')
                    offset = f.tell()
                    word = f.read(4) if kind == FILL else None
                    require(kind != FILL or len(word) == 4, 'short fill')
                    extents.append((at, count, offset, word))
                    f.seek(offset + payload)
                require(f.tell() <= size, 'sparse payload past EOF')
                at += count
            require(at == BLOCKS and f.tell() == size and extents, 'sparse length/trailing data/empty')
            # A filesystem init image, not arbitrary sparse bytes: primary and alternate HFSX headers.
            def sparse_bytes(pos, length):
                for start, count, offset, word in extents:
                    if start * BS <= pos and pos + length <= (start + count) * BS:
                        if word is not None:
                            return (word * ((length + 3) // 4))[:length]
                        f.seek(offset + pos - start * BS)
                        return f.read(length)
                return b''
            for pos in (DATA_FIRST * BS + 1024, BLOCKS * BS - 1024):
                vh = sparse_bytes(pos, 512)
                require(len(vh) == 512 and vh[:2] == b'HX', 'sparse data lacks HFSX volume headers')
                require(struct.unpack_from('>II', vh, 40) == (BS, BLOCKS - DATA_FIRST), 'HFSX size mismatch')
            return 'sparse-data', extents
        require(size > 0 and size % BS == 0, 'raw image is empty or not 4K aligned')
        count = size // BS
        bounds(block, count)
        if block < DATA_FIRST:
            slots = [name for name, region in SLOTS.items() if name != 'data' and region == (block, count)]
            require(len(slots) == 1, 'APFS write must exactly fill root, os or app slot')
            require(hdr[32:36] == b'NXSB' and struct.unpack_from('<IQ', hdr, 36) == (BS, count),
                    'not a raw APFS container of the slot size (no DMG/system.apfs/flashboot here)')
            label = slots[0]
        else:
            bounds(block, count, data=True)
            label = 'raw-data'
        return label, [(block, count, 0, None)]


def pieces(path, extents):
    with path.open('rb') as f:
        for block, count, offset, word in extents:
            for done in range(0, count, CHUNK // BS):
                n = min(count - done, CHUNK // BS)
                if word is None:
                    f.seek(offset + done * BS)
                    data = f.read(n * BS)
                    require(len(data) == n * BS, 'input changed or truncated')
                else:
                    data = word * (n * BS // 4)
                yield block + done, data


class Phone:
    def __init__(self, serial):
        require(bool(serial), '--serial is required for every phone command')
        self.adb = ['adb', '-s', serial]

    def run(self, args):
        r = subprocess.run(self.adb + args, capture_output=True, timeout=3600)
        require(r.returncode == 0, f'adb failed: {(r.stdout + r.stderr).decode(errors="replace")[-600:]}')
        return r.stdout.decode(errors='strict').replace('\r', '')

    def text(self, script):
        # Explicitly no stdin/pty payload and no binary stdout. A sentinel catches su masking sh's exit.
        command = 'set -eu\n' + script + '\nprintf "\\nUMI_OK\\n"\n'
        out = self.run(['shell', 'su -c ' + shlex.quote(command) + ' </dev/null'])
        require(out.endswith('\nUMI_OK\n'), 'root command failed/no success sentinel: ' + out[-600:])
        return out[:-len('\nUMI_OK\n')]

    def guard(self):
        return f'''test "$(readlink -f {DEV})" = {NODE}
test "$(cat /sys/class/block/sda35/partition)" = {PARTN}
test "$(cat /sys/class/block/sda35/start)" = {FIRST * 8}
test "$(cat /sys/class/block/sda35/size)" = {BLOCKS * 8}
test "$(blockdev --getsize64 {DEV})" = {BLOCKS * BS}
test "$(blockdev --getsize64 /dev/block/sda)" = {DISK_BLOCKS * BS}
test "$(blockdev --getss /dev/block/sda)" = {BS}
test "$(sed -n 's/^PARTNAME=//p' /sys/class/block/sda35/uevent)" = darwin
case "$(readlink -f /sys/class/block/sda/device)" in *ufshc*/0:0:0:0|*ufshc*/[0-9]*:[0-9]*:[0-9]*:0) ;; *) exit 9 ;; esac
'''

    def remote_hash(self, path):
        out = self.text('sha256sum ' + shlex.quote(path))
        h = out.split()[0] if out.split() else ''
        require(re.fullmatch('[0-9a-f]{64}', h), 'invalid remote sha256')
        return h

    def pull_checked(self, remote, local, size):
        digest = self.remote_hash(remote)
        self.run(['pull', remote, str(local)])
        require(local.stat().st_size == size, 'pulled file has wrong size')
        with local.open('rb') as f:
            require(hashlib.file_digest(f, 'sha256').hexdigest() == digest, 'pull SHA256 mismatch')

    def identity(self, remote, local):
        self.text(self.guard() + f'dd if=/dev/block/sda of={remote} bs={BS} count=8\nchmod 644 {remote}\n')
        self.pull_checked(remote, local, 8 * BS)
        check_gpt(local.read_bytes())

    def read(self, block, count, remote, local):
        bounds(block, count)
        self.text(self.guard() + f'dd if={DEV} of={remote} bs={BS} skip={block} count={count}\nchmod 644 {remote}\n')
        self.pull_checked(remote, local, count * BS)
        return local.read_bytes()


def operate(args):
    phone = Phone(args.serial)
    OUT.mkdir(parents=True, exist_ok=True)
    # shell-owned, random directory: root's files remain pullable. No fixed tmp file or symlink reuse.
    remote = '/data/local/tmp/darwin-write-umi-' + uuid.uuid4().hex
    phone.run(['shell', 'mkdir -m 700 ' + remote])
    try:
        with tempfile.TemporaryDirectory(prefix='transfer-', dir=OUT) as tmp:
            local = Path(tmp) / 'chunk'
            phone.identity(remote + '/gpt', local)
            audit(f'identity OK: {NODE}, GPT index 34, LUN0, {FIRST}..{LAST}, {UNIQUE}')
            if args.command == 'check':
                return
            if args.command == 'sum':
                bounds(args.block, args.count)
                digest = hashlib.sha256()
                for at in range(args.block, args.block + args.count, CHUNK // BS):
                    n = min(CHUNK // BS, args.block + args.count - at)
                    digest.update(phone.read(at, n, remote + '/read', local))
                audit(f'sha256 blocks {args.block}+{args.count}: {digest.hexdigest()}')
                return
            kind, extents = image_plan(args.image, args.block)
            digest = hashlib.sha256()
            written = 0
            for at, data in pieces(args.image, extents):
                digest.update(data)
                written += len(data)
            audit(f'{args.command}: {args.image} ({kind}), {len(extents)} extents, {written} bytes; '
                  f'SHA256 of written bytes in order {digest.hexdigest()}')
            for at, n, _, _ in extents:
                audit(f'  darwin blocks {at}..{at + n - 1} ({n} blocks)')
            audit(f'dd destination is ONLY {DEV}; 16 MiB push + dd + pull/readback per piece')
            if args.command == 'plan':
                return
            require(os.environ.get('DARWIN_WRITE_UMI') == 'yes', 'write requires DARWIN_WRITE_UMI=yes')
            actual = hashlib.sha256()
            for at, data in pieces(args.image, extents):
                local.write_bytes(data)
                phone.run(['push', str(local), remote + '/input'])
                require(phone.remote_hash(remote + '/input') == sha(data), 'push SHA256 mismatch')
                # Full GPT revalidated immediately before each bounded dd; sysfs rechecked in dd's shell.
                phone.identity(remote + '/gpt', Path(tmp) / 'gpt')
                n = len(data) // BS
                phone.text(phone.guard() + f'''test "$(stat -c %s {remote}/input)" = {len(data)}
dd if={remote}/input of={DEV} bs={BS} seek={at} count={n} conv=notrunc,fsync
''')
                back = phone.read(at, n, remote + '/read', local)
                require(sha(back) == sha(data), f'READBACK MISMATCH at {at}: stop, do not boot')
                actual.update(data)
                audit(f'verified blocks {at}+{n}: {sha(data)}')
            require(actual.digest() == digest.digest(), 'input changed after plan; stop, do not boot')
            audit('write complete; every written byte read back and SHA256 matched')
    finally:
        phone.text('rm -f ' + remote + '/input ' + remote + '/read ' + remote + '/gpt\nrmdir ' + remote)


def selftest(path):
    head = path.read_bytes()
    check_gpt(head)
    tests = 1
    def refuse(action):
        nonlocal tests
        try:
            action()
        except (Refused, UnicodeError):
            tests += 1
            return
        raise AssertionError('unsafe input accepted')
    def mutate(offset, data, repair=False):
        b = bytearray(head)
        b[offset:offset + len(data)] = data
        if repair:
            struct.pack_into('<I', b, BS + 88, binascii.crc32(b[2 * BS:4 * BS]) & 0xffffffff)
            struct.pack_into('<I', b, BS + 16, 0)
            struct.pack_into('<I', b, BS + 16, binascii.crc32(b[BS:BS + 92]) & 0xffffffff)
        return bytes(b)
    e = 2 * BS + (PARTN - 1) * 128
    for off in (BS, BS + 16, 2 * BS):
        refuse(lambda off=off: check_gpt(mutate(off, bytes([head[off] ^ 1]))))
    for off, value in ((e, bytes(16)), (e + 16, bytes(16)), (e + 32, struct.pack('<Q', FIRST + 1)),
                       (e + 40, struct.pack('<Q', LAST - 1)), (e + 56, 'wrong'.encode('utf-16le')),
                       (2 * BS + 33 * 128 + 40, struct.pack('<Q', FIRST))):
        refuse(lambda off=off, value=value: check_gpt(mutate(off, value, True)))
    for b, n in ((-1, 1), (0, 0), (BLOCKS, 1), (DATA_FIRST - 1, 1)):
        refuse(lambda b=b, n=n: bounds(b, n, data=True))
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='selftest-', dir=OUT) as tmp:
        image = Path(tmp) / 'image'
        image.write_bytes(bytes(BS))
        refuse(lambda: image_plan(image, 0))
        assert image_plan(image, DATA_FIRST)[0] == 'raw-data'
        refuse(lambda: image_plan(image, DATA_FIRST - 1))
        refuse(lambda: image_plan(image, BLOCKS))
        # Android sparse attempting to replace even one system block must be refused before any adb.
        image.write_bytes(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, BLOCKS, 2, 0) +
                          struct.pack('<HHII', FILL, 0, 1, 16) + bytes(4) +
                          struct.pack('<HHII', DONT_CARE, 0, BLOCKS - 1, 12))
        refuse(lambda: image_plan(image, 0))
        refuse(lambda: image_plan(image, DATA_FIRST))
    print(f'selftest: {tests} checks passed against {path}; no adb/fastboot invoked')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--serial', default=os.environ.get('ANDROID_SERIAL'))
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('check')
    p = commands.add_parser('selftest')
    p.add_argument('gpt', nargs='?', type=Path, default=GPT_HEAD)
    for cmd in ('plan', 'write'):
        p = commands.add_parser(cmd)
        p.add_argument('image', type=Path)
        p.add_argument('block', type=int)
    p = commands.add_parser('sum')
    p.add_argument('block', type=int)
    p.add_argument('count', type=int)
    args = parser.parse_args()
    try:
        if args.command == 'selftest':
            selftest(args.gpt)
        else:
            # Local validation before even connecting.
            if args.command in ('plan', 'write'):
                image_plan(args.image, args.block)
            if args.command == 'write':
                require(os.environ.get('DARWIN_WRITE_UMI') == 'yes', 'write requires DARWIN_WRITE_UMI=yes')
            if args.command == 'sum':
                bounds(args.block, args.count)
            operate(args)
    except (Refused, OSError, ValueError, subprocess.SubprocessError) as e:
        audit('REFUSED: ' + str(e))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

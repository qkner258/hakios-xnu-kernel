#!/usr/bin/env python3
"""Read USB diagnostic files from HFS+, never modify the volume.

--image reads a local HFS image (or darwin image with --offset).
--android is SUPERVISOR ONLY: bounded dd reads from darwin to /sdcard,
then adb pull. No mount, partition writes, exec-out or raw var_store access.
Supports inline and extents-overflow B-tree mappings, including self-extension.
No journal replay is performed.
"""
import argparse
from collections import OrderedDict
from pathlib import Path
import re
import struct
import subprocess
import tempfile

DATA_OFFSET = 2042368 * 4096
MAX_LOG = 1280 * 1024

def decode_snapshot(data):
    if len(data) < 48:
        raise ValueError('short snapshot')
    header = data[:24]
    magic, version, epoch, seq, length, expected = struct.unpack('<6I', header)
    if magic != 0x32474f4c or version != 1 or not seq or length > MAX_LOG:
        raise ValueError('bad snapshot header')
    if data[24 + length:48 + length] != header:
        raise ValueError('torn snapshot (header/trailer mismatch)')
    log = data[24:24 + length]
    checksum = 2166136261
    for byte in log:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    if checksum != expected:
        raise ValueError('torn snapshot (checksum mismatch)')
    return epoch, seq, log

class Image:
    def __init__(self, path): self.file = path.open('rb')
    def read(self, offset, size):
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size: raise ValueError('short image read')
        return data
    def close(self): self.file.close()

class Android:
    def __init__(self, serial):
        self.serial = serial
        self.cache = OrderedDict()
        self.read_bytes = 0
        self.temp = tempfile.TemporaryDirectory(prefix='umi-usb-hfs-')
        self.remote = '/sdcard/umi-usb-hfs-read.bin'
    def adb(self, *args):
        subprocess.run(['adb', '-s', self.serial, *args], check=True, stdout=subprocess.DEVNULL)
    def read(self, offset, size):
        result = bytearray()
        while size:
            # Read 1 MiB pages; cache only 8 MiB locally, total capped at 128 MiB.
            base = offset & ~((1 << 20) - 1)
            if base not in self.cache:
                if self.read_bytes + (1 << 20) > 128 << 20:
                    raise ValueError('128 MiB read budget exhausted')
                self.adb('shell', 'su', '-c',
                         f'dd if=/dev/block/by-name/darwin of={self.remote} bs=4096 skip={base // 4096} count=256')
                local = Path(self.temp.name) / 'page.bin'
                self.adb('pull', self.remote, str(local))
                data = local.read_bytes()
                local.unlink()
                if len(data) != 1 << 20: raise ValueError('short Android page read')
                self.cache[base] = data
                self.read_bytes += len(data)
                if len(self.cache) > 8: self.cache.popitem(last=False)
            self.cache.move_to_end(base)
            at = offset - base
            count = min(size, (1 << 20) - at)
            result += self.cache[base][at:at + count]
            offset += count
            size -= count
        return bytes(result)
    def close(self):
        try: self.adb('shell', 'rm', '-f', self.remote)
        finally: self.temp.cleanup()

class Fork:
    def __init__(self, hfs, raw, file_id=None, fork_type=0):
        self.hfs, self.file_id, self.fork_type = hfs, file_id, fork_type
        self.size, _, self.blocks = struct.unpack_from('>QII', raw)
        if self.size > self.blocks * hfs.blocksize:
            raise ValueError('fork logical size exceeds allocated blocks')
        self.extents = []
        self.mapped = 0
        self.append_extents(raw[16:80])

    def append_extents(self, raw):
        if len(raw) != 64: raise ValueError('short extent record')
        extents, ended = [], False
        for i in range(8):
            start, count = struct.unpack_from('>II', raw, i * 8)
            if not count:
                ended = True
                continue
            if ended or start + count > self.hfs.blocks:
                raise ValueError('invalid extent / outside HFS volume')
            extents.append((start, count))
        added = sum(n for _, n in extents)
        if self.mapped + added > self.blocks:
            raise ValueError('extent record exceeds allocated fork blocks')
        self.extents.extend(extents)
        self.mapped += added
        return added

    def read(self, off, size):
        if off < 0 or size < 0 or off + size > self.size: raise ValueError('fork read out of bounds')
        if not size: return b''
        end = (off + size + self.hfs.blocksize - 1) // self.hfs.blocksize
        while end > self.mapped:
            if self.file_id is None or self.hfs.overflow is None:
                raise ValueError('overflow fork requires file ID and extents B-tree')
            before = self.mapped
            if self.file_id == 3:
                self.hfs.overflow.extend_self()
            else:
                key = (self.file_id, self.fork_type, before)
                raw = self.hfs.overflow.lookup(key)
                if raw is None: raise ValueError(f'missing overflow extent key={key}')
                self.append_extents(raw)
            if self.mapped <= before: raise ValueError('overflow extent made no progress')
        result = bytearray()
        for block, count in self.extents:
            length = count * self.hfs.blocksize
            if off >= length:
                off -= length
                continue
            take = min(size, length - off)
            result += self.hfs.read(block * self.hfs.blocksize + off, take)
            size -= take
            off = 0
            if not size: return bytes(result)
        raise ValueError('incomplete extent mapping')


def node_records(raw):
    if len(raw) < 14: raise ValueError('short B-tree node')
    next_node, _, kind, height, count = struct.unpack_from('>IIbBH', raw)
    if count > (len(raw) - 14) // 4: raise ValueError('invalid B-tree record count')
    records = []
    for i in range(count):
        start = struct.unpack_from('>H', raw, len(raw) - 2 * (i + 1))[0]
        end = struct.unpack_from('>H', raw, len(raw) - 2 * (i + 2))[0]
        if not 14 <= start < end <= len(raw) - 2 * (count + 1):
            raise ValueError('invalid B-tree record offsets')
        records.append(raw[start:end])
    return next_node, kind, height, records


class ExtentTree:
    def __init__(self, fork):
        self.fork = fork
        header = fork.read(0, 64)
        self.root = struct.unpack_from('>I', header, 16)[0]
        self.node_size = struct.unpack_from('>H', header, 32)[0]
        self.total_nodes = struct.unpack_from('>I', header, 36)[0]
        if not 512 <= self.node_size <= 32768 or self.node_size & (self.node_size - 1):
            raise ValueError('invalid overflow B-tree node size')
        if not self.total_nodes or self.total_nodes * self.node_size > fork.size:
            raise ValueError('invalid overflow B-tree node count')
        records = node_records(fork.read(0, self.node_size))[3]
        if len(records) < 3: raise ValueError('missing overflow B-tree allocation map')
        self.node_map = records[2]
        self.self_scan_node = 1
        self.self_records = {}

    @staticmethod
    def record(raw):
        if len(raw) < 12 or struct.unpack_from('>H', raw)[0] != 10:
            raise ValueError('invalid extent B-tree key')
        fork_type, pad, file_id, start = struct.unpack_from('>BBII', raw, 2)
        if fork_type not in (0, 255) or pad: raise ValueError('invalid extent fork type')
        # HFS order is fileID, forkType, startBlock (not the on-disk field order).
        return (file_id, fork_type, start), raw[12:]

    def node(self, index):
        if not 0 < index < self.total_nodes: raise ValueError('overflow B-tree child out of bounds')
        return node_records(self.fork.read(index * self.node_size, self.node_size))

    def lookup(self, wanted):
        node, seen, previous_height = self.root, set(), None
        while node:
            if node in seen: raise ValueError('overflow B-tree index cycle')
            seen.add(node)
            _, kind, height, raw_records = self.node(node)
            if kind not in (0, -1) or not height or (previous_height is not None and height != previous_height - 1):
                raise ValueError('invalid overflow B-tree height/type')
            if kind == -1 and height != 1: raise ValueError('invalid overflow leaf height')
            records = [self.record(raw) for raw in raw_records]
            if any(a[0] >= b[0] for a, b in zip(records, records[1:])):
                raise ValueError('unsorted/duplicate overflow keys')
            if kind == -1:
                for key, data in records:
                    if key == wanted:
                        if len(data) != 64: raise ValueError('invalid overflow extent data')
                        return data
                return None
            eligible = [data for key, data in records if key <= wanted]
            if not eligible: return None
            data = eligible[-1]
            if len(data) != 4: raise ValueError('invalid overflow index child')
            node = struct.unpack('>I', data)[0]
            previous_height = height
        return None

    def extend_self(self):
        # Bootstrap file ID 3 from accessible leaf nodes. Reading the root can
        # itself require an overflow extent, so do not recursively search it.
        wanted = self.fork.mapped
        accessible = min(self.total_nodes, self.fork.mapped * self.fork.hfs.blocksize // self.node_size)
        while wanted not in self.self_records and self.self_scan_node < accessible:
            index = self.self_scan_node
            self.self_scan_node += 1
            if index // 8 >= len(self.node_map):
                raise ValueError('extents-file bootstrap exceeds header allocation map')
            if not (self.node_map[index // 8] & (0x80 >> (index % 8))): continue
            # Free B-tree nodes need not have valid record offsets. Inspect the
            # descriptor first, and parse only leaf nodes (height == 1).
            raw = self.fork.read(index * self.node_size, self.node_size)
            if raw[8:10] != bytes((255, 1)): continue
            for raw_record in node_records(raw)[3]:
                key, data = self.record(raw_record)
                if key[:2] == (3, 0):
                    if len(data) != 64 or key[2] in self.self_records:
                        raise ValueError('invalid/duplicate extents-file self record')
                    self.self_records[key[2]] = data
        raw = self.self_records.pop(wanted, None)
        if raw is None:
            raise ValueError(f'extents-file bootstrap cannot reach continuation at block {wanted}')
        self.fork.append_extents(raw)

class HFS:
    def __init__(self, backend, offset):
        self.backend, self.offset = backend, offset
        header = backend.read(offset + 1024, 512)
        if header[:2] not in (b'H+', b'HX'): raise ValueError('not HFS+/HFSX at supplied offset')
        self.blocksize, self.blocks = struct.unpack_from('>II', header, 40)
        if self.blocksize != 4096: raise ValueError('expected UMI HFS 4096-byte blocks')
        self.extents_file = Fork(self, header[192:272], 3)
        self.overflow = ExtentTree(self.extents_file) if self.extents_file.size else None
        self.catalog = Fork(self, header[272:352], 4)
    def read(self, off, size):
        if off < 0 or off + size > self.blocks * self.blocksize: raise ValueError('volume read out of bounds')
        return self.backend.read(self.offset + off, size)
    def usb_files(self):
        header = self.catalog.read(0, 64)
        first = struct.unpack_from('>I', header, 24)[0]
        node_size = struct.unpack_from('>H', header, 32)[0]
        if node_size < 512 or node_size > 32768 or node_size & (node_size - 1):
            raise ValueError('invalid catalog node size')
        node, seen, db_id, candidates = first, set(), None, []
        while node:
            if node in seen: raise ValueError('catalog leaf loop')
            seen.add(node)
            raw = self.catalog.read(node * node_size, node_size)
            next_node, kind, height, records = node_records(raw)
            if kind != -1 or height != 1: raise ValueError('invalid catalog leaf')
            for rec in records:
                keylen, parent, chars = struct.unpack_from('>HIH', rec)
                if keylen != 6 + chars * 2 or keylen + 4 > len(rec): raise ValueError('invalid catalog key')
                name = rec[8:8 + chars * 2].decode('utf-16-be')
                data = rec[2 + keylen:]
                record_type = struct.unpack_from('>H', data)[0]
                if record_type == 1 and parent == 2 and name == 'db':
                    db_id = struct.unpack_from('>I', data, 8)[0]
                if record_type == 2 and re.fullmatch(r'hakios-usb-\d+-[01]\.bin', name):
                    if len(data) < 168: raise ValueError('short file record')
                    candidates.append((parent, name, struct.unpack_from('>I', data, 8)[0], data[88:168]))
            node = next_node
        return [(name, Fork(self, fork, file_id)) for parent, name, file_id, fork in candidates if parent == db_id]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument('--image', type=Path)
    source.add_argument('--android', action='store_true')
    source.add_argument('--decode', type=Path, help='decode already extracted snapshot; no HFS or USB access')
    p.add_argument('--serial', default='b54a311e')
    p.add_argument('--offset', type=int, help='bytes; default 0 for image, 8365539328 for Android darwin')
    p.add_argument('--epoch', type=int, help='default selects newest epoch in catalog')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.decode:
        epoch, seq, log = decode_snapshot(args.decode.read_bytes())
        dest = args.output / (args.decode.stem + '.log')
        dest.write_bytes(log)
        print(f'epoch={epoch} sequence={seq} bytes={len(log)} {dest}')
        return
    backend = Android(args.serial) if args.android else Image(args.image)
    try:
        offset = args.offset if args.offset is not None else DATA_OFFSET if args.android else 0
        if args.android and offset != DATA_OFFSET: raise ValueError('Android offset must be UMI HFS data start')
        files = HFS(backend, offset).usb_files()
        if not files: raise ValueError('no /db/hakios-usb files; HFS logger may not have reached its mount')
        epoch = args.epoch if args.epoch is not None else max(int(name.split('-')[2]) for name, _ in files)
        valid = 0
        for name, fork in files:
            if int(name.split('-')[2]) != epoch: continue
            if fork.size > MAX_LOG + 48: raise ValueError('oversized log file')
            raw = fork.read(0, fork.size)
            (args.output / name).write_bytes(raw)
            try:
                stored_epoch, seq, log = decode_snapshot(raw)
                if stored_epoch != epoch: raise ValueError('filename/header epoch mismatch')
            except ValueError as exc:
                print(f'{name}: {exc}; raw file retained')
                continue
            dest = args.output / (name[:-4] + '.log')
            dest.write_bytes(log)
            valid += 1
            print(f'epoch={epoch} sequence={seq} bytes={len(log)} {dest}')
        if not valid: raise ValueError('no valid snapshot for selected epoch')
    finally:
        backend.close()

if __name__ == '__main__':
    try: main()
    except (ValueError, OSError, struct.error, subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc))

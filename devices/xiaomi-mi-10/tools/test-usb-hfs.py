#!/usr/bin/env python3
"""Host-only synthetic HFS catalog and snapshot corruption tests."""
import importlib.util
from pathlib import Path
import struct
spec = importlib.util.spec_from_file_location('hfs', Path(__file__).with_name('read-usb-hfs.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)
log = b'boot\n[usb] test\n' * 600
checksum = 2166136261
for b in log: checksum = ((checksum ^ b) * 16777619) & 0xffffffff
header = struct.pack('<6I', 0x32474f4c, 1, 12345, 9, len(log), checksum)
snapshot = header + log + header
assert h.decode_snapshot(snapshot) == (12345, 9, log)
for raw in (snapshot[:-1], snapshot[:100] + b'!' + snapshot[101:], b'\0' * 48):
    try:
        h.decode_snapshot(raw)
        raise AssertionError('corrupt snapshot accepted')
    except ValueError: pass
image = bytearray(32 * 4096)
image[1024:1026] = b'HX'
struct.pack_into('>II', image, 1064, 4096, 32)
def fork(size, extents):
    raw = bytearray(80)
    struct.pack_into('>QII', raw, 0, size, 0, sum(n for _, n in extents))
    for i, (start, count) in enumerate(extents):
        struct.pack_into('>II', raw, 16 + 8*i, start, count)
    return raw
image[1296:1376] = fork(8192, [(4, 2)])
struct.pack_into('>I', image, 4*4096 + 24, 1)
struct.pack_into('>H', image, 4*4096 + 32, 4096)
def record(parent, name, data):
    text = name.encode('utf-16-be')
    return struct.pack('>HIH', 6 + len(text), parent, len(text)//2) + text + data
folder = bytearray(88)
struct.pack_into('>H', folder, 0, 1)
struct.pack_into('>I', folder, 8, 42)
file = bytearray(248)
struct.pack_into('>H', file, 0, 2)
file[88:168] = fork(len(snapshot), [(8, 1), (12, 2)])
records = [record(2, 'db', folder), record(42, 'hakios-usb-12345-1.bin', file)]
leaf = bytearray(4096)
struct.pack_into('>IIbBH', leaf, 0, 0, 0, -1, 1, len(records))
at = 14
for i, rec in enumerate(records):
    struct.pack_into('>H', leaf, 4096 - 2*(i+1), at)
    leaf[at:at + len(rec)] = rec
    at += len(rec)
struct.pack_into('>H', leaf, 4096 - 2*(len(records)+1), at)
image[5*4096:6*4096] = leaf
image[8*4096:9*4096] = snapshot[:4096]
image[12*4096:12*4096 + len(snapshot)-4096] = snapshot[4096:]
class Memory:
    def read(self, offset, size): return bytes(image[offset:offset+size])
volume = h.HFS(Memory(), 0)
assert h.Fork(volume, fork(0, [])).read(0, 0) == b''
files = volume.usb_files()
assert len(files) == 1 and files[0][0] == 'hakios-usb-12345-1.bin'
assert h.decode_snapshot(files[0][1].read(0, len(snapshot))) == (12345, 9, log)
# Lazy extents: even an allocated-but-unused continuation must not prevent
# reading bytes that are already mapped inline.
overflow = fork(8192, [(8, 1)])
struct.pack_into('>I', overflow, 12, 2)
f = h.Fork(volume, overflow)
assert len(f.read(0, 4)) == 4
try:
    f.read(4096, 1)
    raise AssertionError('missing file ID accepted')
except ValueError: pass

# Real B-tree shapes: extents root is an index outside the inline extents.
# Its continuation record is in an allocated inline leaf (file ID 3).
# Catalog (ID 4) and log data (ID 43) both cross 8 inline extents.
def node(kind, height, records, next_node=0):
    raw = bytearray(4096)
    struct.pack_into('>IIbBH', raw, 0, next_node, 0, kind, height, len(records))
    pos = 14
    for i, rec in enumerate(records):
        struct.pack_into('>H', raw, 4096 - 2*(i+1), pos)
        raw[pos:pos+len(rec)] = rec
        pos += len(rec)
    struct.pack_into('>H', raw, 4096 - 2*(len(records)+1), pos)
    return raw

def extent_key(file_id, start, fork_type=0):
    return struct.pack('>HBBII', 10, fork_type, 0, file_id, start)

def extent_data(pairs): return fork(0, pairs)[16:80]

image = bytearray(128 * 4096)
image[1024:1026] = b'HX'
struct.pack_into('>II', image, 1064, 4096, 128)
cat_extents = [(b, 1) for b in (4, 5, 6, 7, 9, 11, 13, 15)]
catfork = fork(10*4096, cat_extents)
struct.pack_into('>I', catfork, 12, 10)
image[1296:1376] = catfork
extfork = fork(4*4096, [(30, 2)])
struct.pack_into('>I', extfork, 12, 4)
image[1216:1296] = extfork
bt_header = bytearray(106)
struct.pack_into('>HIIIIHHII', bt_header, 0, 2, 3, 3, 1, 1, 4096, 10, 4, 1)
image[30*4096:31*4096] = node(1, 0, [bt_header, bytes(128), bytes([0xd0])]) # nodes 0,1,3
self_rec = extent_key(3, 2) + extent_data([(33, 2)])
cat_rec = extent_key(4, 8) + extent_data([(24, 2)])
file_rec = extent_key(43, 8) + extent_data([(40, 4)])
image[31*4096:32*4096] = node(-1, 1, [self_rec, cat_rec, file_rec])
image[34*4096:35*4096] = node(0, 2, [extent_key(3, 2) + struct.pack('>I', 1)])
struct.pack_into('>I', image, 4*4096 + 24, 9)
struct.pack_into('>H', image, 4*4096 + 32, 4096)
log = b'boot\n[usb] overflow test\n' * 1800
checksum = 2166136261
for b in log: checksum = ((checksum ^ b) * 16777619) & 0xffffffff
header = struct.pack('<6I', 0x32474f4c, 1, 1794243689, 60, len(log), checksum)
snapshot = header + log + header
file_extents = [(b, 1) for b in (8, 10, 12, 14, 16, 18, 20, 22)]
forkraw = fork(len(snapshot), file_extents)
struct.pack_into('>I', forkraw, 12, 12)
struct.pack_into('>I', file, 8, 43)
file[88:168] = forkraw
image[25*4096:26*4096] = node(-1, 1, [record(2, 'db', folder), record(42, 'hakios-usb-1794243689-1.bin', file)])
for i, (block, _) in enumerate(file_extents):
    image[block*4096:(block+1)*4096] = snapshot[i*4096:(i+1)*4096]
image[40*4096:40*4096+len(snapshot)-32768] = snapshot[32768:]
volume = h.HFS(Memory(), 0)
files = volume.usb_files()
assert volume.extents_file.mapped == 4 and volume.catalog.mapped == 10
assert len(files) == 1 and files[0][1].file_id == 43
assert h.decode_snapshot(files[0][1].read(0, len(snapshot))) == (1794243689, 60, log)
assert volume.overflow.lookup((43, 255, 8)) is None
assert volume.overflow.lookup((44, 0, 8)) is None
# A stale self-record in a FREE node must not bootstrap the extents file.
map_offset = struct.unpack_from('>H', image, 31*4096 - 6)[0]
image[30*4096+map_offset] = 0x90 # nodes 0,3; leaf 1 free
try:
    h.HFS(Memory(), 0).usb_files()
    raise AssertionError('free-node self-record used')
except ValueError as exc:
    assert 'bootstrap' in str(exc)
image[30*4096+map_offset] = 0xd0
# Index loops and out-of-volume overflow records fail without infinite IO.
image[34*4096:35*4096] = node(0, 2, [extent_key(3, 2) + struct.pack('>I', 3)])
try:
    h.HFS(Memory(), 0).usb_files()
    raise AssertionError('index cycle accepted')
except ValueError as exc:
    assert 'cycle' in str(exc)
image[34*4096:35*4096] = node(0, 2, [extent_key(3, 2) + struct.pack('>I', 1)])
image[31*4096:32*4096] = node(-1, 1, [self_rec, cat_rec, extent_key(43, 8)+extent_data([(127, 4)])])
try:
    files = h.HFS(Memory(), 0).usb_files()
    files[0][1].read(0, len(snapshot))
    raise AssertionError('out-of-volume extent accepted')
except ValueError as exc:
    assert 'outside HFS' in str(exc)
print('PASS: HFSX catalog + data overflow, indexed lookup, extents-file self bootstrap/allocation map, missing keys, cycles, physical bounds, torn snapshots (no device access)')

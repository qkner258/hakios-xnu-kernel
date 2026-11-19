#!/usr/bin/env python3
"""Print the protobuf schemas compiled into an iOS binary (gpsd, locationd) as .proto text.

usage: gpsd-proto-dump.py BINARY [OUTDIR]

Generated C++ protobuf code keeps each .proto file's FileDescriptorProto, serialized, in the
binary. This finds every one whose name ends in ".proto" and decodes it with a small wire-format
reader (no protobuf package needed). With OUTDIR, each schema is written to OUTDIR/<name>;
without it, all of them go to standard output.

The schemas are Apple's (they come out of Apple's binary): write them under out/, never into the
repository. LOCATION-umi.md uses GpsdProtocol.proto and GnssTypes.proto from /usr/libexec/gpsd:
they are the interface between locationd (CLGnssDaemonDevice) and gpsd on the com.apple.gpsd service.
"""
import os
import re
import sys

TYPES = {1: 'double', 2: 'float', 3: 'int64', 4: 'uint64', 5: 'int32', 6: 'fixed64', 7: 'fixed32',
         8: 'bool', 9: 'string', 10: 'group', 11: 'message', 12: 'bytes', 13: 'uint32', 14: 'enum',
         15: 'sfixed32', 16: 'sfixed64', 17: 'sint32', 18: 'sint64'}
LABELS = {1: 'optional', 2: 'required', 3: 'repeated'}
# FileDescriptorProto: field number -> wire type
FILE_FIELDS = {1: 2, 2: 2, 3: 2, 4: 2, 5: 2, 6: 2, 7: 2, 8: 2, 9: 2, 10: 0, 11: 0, 12: 2, 14: 0}


def varint(b, i):
    v = s = 0
    while True:
        if i >= len(b) or s > 63:
            raise ValueError('bad varint')
        c = b[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if not c & 0x80:
            return v, i


def fields(b):
    """(number, wire type, value) of a message's fields; value is bytes for LEN, int otherwise."""
    i = 0
    while i < len(b):
        tag, i = varint(b, i)
        num, wt = tag >> 3, tag & 7
        if wt == 0:
            v, i = varint(b, i)
        elif wt == 2:
            n, i = varint(b, i)
            v, i = b[i:i + n], i + n
            if i > len(b):
                raise ValueError('truncated')
        elif wt == 1:
            v, i = int.from_bytes(b[i:i + 8], 'little'), i + 8
        elif wt == 5:
            v, i = int.from_bytes(b[i:i + 4], 'little'), i + 4
        else:
            raise ValueError(f'wire type {wt}')
        yield num, wt, v


def file_extent(b, start):
    """End of a FileDescriptorProto that starts at b[start]: fields are read while they are valid."""
    i = end = start
    while i < len(b):
        try:
            tag, j = varint(b, i)
        except ValueError:
            break
        num, wt = tag >> 3, tag & 7
        if FILE_FIELDS.get(num) != wt:
            break
        try:
            if wt == 0:
                _, j = varint(b, j)
            else:
                n, j = varint(b, j)
                j += n
        except ValueError:
            break
        if j > len(b):
            break
        i = end = j
    return end


def text(v):
    return v.decode('utf-8', 'replace')


def enum(b, ind):
    name, values = '', []
    for num, _, v in fields(b):
        if num == 1:
            name = text(v)
        elif num == 2:
            vn, vv = '', 0
            for n2, _, v2 in fields(v):
                if n2 == 1:
                    vn = text(v2)
                elif n2 == 2:
                    vv = v2 - (1 << 64) if v2 >= 1 << 63 else v2
                    vv = vv - (1 << 32) if 1 << 31 <= vv < 1 << 32 else vv
            values.append(f'{ind}  {vn} = {vv};')
    return [f'{ind}enum {name} {{'] + values + [f'{ind}}}']


def field(b, ind, oneofs):
    f = {'label': 1, 'type': 0, 'type_name': '', 'name': '', 'number': 0, 'default': None, 'oneof': None}
    for num, _, v in fields(b):
        if num == 1:
            f['name'] = text(v)
        elif num == 3:
            f['number'] = v
        elif num == 4:
            f['label'] = v
        elif num == 5:
            f['type'] = v
        elif num == 6:
            f['type_name'] = text(v)
        elif num == 7:
            f['default'] = text(v)
        elif num == 9:
            f['oneof'] = v
    t = f['type_name'] or TYPES.get(f['type'], f'type{f["type"]}')
    label = '' if f['oneof'] is not None else LABELS.get(f['label'], '') + ' '
    extra = f' [default = {f["default"]}]' if f['default'] is not None else ''
    return f['oneof'], f'{ind}{label}{t} {f["name"]} = {f["number"]}{extra};'


def message(b, ind):
    name, body, oneof_names, members = '', [], [], {}
    plain = []
    for num, _, v in fields(b):
        if num == 1:
            name = text(v)
        elif num == 2:
            o, line = field(v, ind + '  ', oneof_names)
            if o is None:
                plain.append(line)
            else:
                members.setdefault(o, []).append('  ' + line)
        elif num == 3:
            body += message(v, ind + '  ')
        elif num == 4:
            body += enum(v, ind + '  ')
        elif num == 8:
            oneof_names.append(next((text(x) for n2, _, x in fields(v) if n2 == 1), ''))
    for k, n in enumerate(oneof_names):
        plain += [f'{ind}  oneof {n} {{'] + members.get(k, []) + [f'{ind}  }}']
    return [f'{ind}message {name} {{'] + body + plain + [f'{ind}}}']


def render(b):
    name, out = '', []
    head = []
    for num, _, v in fields(b):
        if num == 1:
            name = text(v)
        elif num == 2:
            head.append(f'package {text(v)};')
        elif num == 3:
            head.append(f'import "{text(v)}";')
        elif num == 12:
            head.insert(0, f'syntax = "{text(v)}";')
        elif num == 4:
            out += message(v, '')
        elif num == 5:
            out += enum(v, '')
    return name, '\n'.join([f'// {name}, decoded from the binary\'s serialized FileDescriptorProto'] + head + [''] + out) + '\n'


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    seen = set()
    for m in re.finditer(rb'\x0a([\x01-\x7f])([A-Za-z0-9_/.\-]{1,126}\.proto)', data):
        if m.group(1)[0] != len(m.group(2)):
            continue
        start = m.start()
        blob = data[start:file_extent(data, start)]
        try:
            name, proto = render(blob)
        except (ValueError, IndexError):
            continue
        if name in seen or 'message ' not in proto and 'enum ' not in proto:
            continue
        seen.add(name)
        if len(sys.argv) == 3:
            os.makedirs(sys.argv[2], exist_ok=True)
            with open(os.path.join(sys.argv[2], os.path.basename(name)), 'w') as f:
                f.write(proto)
            print(f'{name}: {len(blob)} bytes')
        else:
            sys.stdout.write(proto + '\n')


if __name__ == '__main__':
    main()

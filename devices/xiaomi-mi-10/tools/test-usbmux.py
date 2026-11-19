#!/usr/bin/env python3
"""Host-only: run the real usbmux.c daemon (ASan/UBSan) against a scripted usbmuxd.

A stream socket stands in for /dev/hakios-usbmux, which is also a byte stream
on the OUT side. The script speaks usbmuxd's side of the wire format: VERSION
with the 8-byte header, SETUP, then v2 TCP with sequence numbers and th_win<<8.
Local echo and banner servers on 127.0.0.1 stand in for lockdownd.
"""
from pathlib import Path
import os, queue, socket, struct, subprocess, tempfile, threading

SYN, RST, ACK, FIN = 2, 4, 0x10, 1
here = Path(__file__).resolve().parent


def server(handler):
    s = socket.socket(); s.bind(('127.0.0.1', 0)); s.listen(8)
    def loop():
        while True:
            c, _ = s.accept()
            threading.Thread(target=handler, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return s.getsockname()[1]


def echo(c):
    while (d := c.recv(65536)):
        c.sendall(d)
    c.close()


closed = threading.Event()
def banner(c):
    c.sendall(b'lockdown-banner'); c.close(); closed.set()


class Host:
    """usbmuxd keeps IN reads posted; a reader thread does the same here."""
    def __init__(self, sock):
        self.s, self.buf, self.tx, self.expect = sock, b'', 0, 0
        self.chunks = queue.Queue()
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        try:
            while (d := self.s.recv(65536)):
                self.chunks.put(d)
        except OSError:
            pass

    def read(self, n):
        while len(self.buf) < n:
            self.buf += self.chunks.get(timeout=10)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def packet(self):
        proto, length = struct.unpack('>II', self.read(8))
        rest = self.read(length - 8)
        assert length <= 16384, length                      # one IN transfer each
        if proto == 0:
            return proto, rest, None
        magic, tx, rx = struct.unpack('>IHH', rest[:8])
        assert magic == 0xfeedface and tx == self.expect, (hex(magic), tx, self.expect)
        self.expect = (self.expect + 1) & 0xffff
        return proto, rest[8:], rx

    def send(self, proto, body):
        hdr = struct.pack('>IIIHH', proto, 16 + len(body), 0xfeedface, self.tx, 0xffff)
        self.tx += 1
        self.s.sendall(hdr + body)

    def tcp(self, sport, dport, seq, ack, flags, data=b'', win=0x200):
        self.send(6, struct.pack('>HHIIBBHHH', sport, dport, seq, ack, 5 << 4, flags, win, 0, 0) + data)

    def segment(self):
        proto, body, _ = self.packet(); assert proto == 6, proto
        sport, dport, seq, ack, off, flags, win = struct.unpack('>HHIIBBH', body[:16])
        return dict(sport=sport, dport=dport, seq=seq, ack=ack, flags=flags, win=win << 8,
                    data=body[(off >> 4) * 4:])


def main():
    with tempfile.TemporaryDirectory(prefix='umi-usbmux-', dir=os.environ.get('TMPDIR')) as tmp:
        exe = Path(tmp) / 'usbmux'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', str(here / 'usbmux.c'), '-o', str(exe)], check=True)
        echo_port, banner_port = server(echo), server(banner)
        unused = socket.socket(); unused.bind(('127.0.0.1', 0)); dead_port = unused.getsockname()[1]; unused.close()
        host_end, dev_end = socket.socketpair()
        daemon = subprocess.Popen([str(exe), str(dev_end.fileno())], pass_fds=[dev_end.fileno()],
                                  stderr=subprocess.PIPE, text=True)
        dev_end.close()
        h = Host(host_end)
        try:
            # Apple usbmuxd: VERSION major 2, features 8 (TCP resend), 8-byte header.
            h.s.sendall(struct.pack('>IIIII', 0, 20, 2, 8, 0))
            assert h.packet() == (0, struct.pack('>III', 2, 0, 0), None)
            h.send(2, b'\x07')                                   # SETUP: ignored
            # Refused port -> RST|ACK acknowledging the SYN.
            h.tcp(4000, dead_port, 0, 0, SYN)
            r = h.segment(); assert r['flags'] & RST and r['ack'] == 1, r
            # Connect, data both ways, ACKs, and seq/ack bookkeeping.
            h.tcp(4001, echo_port, 0, 0, SYN)
            r = h.segment(); assert r['flags'] == SYN | ACK and r['seq'] == 0 and r['ack'] == 1, r
            assert (r['sport'], r['dport']) == (echo_port, 4001) and r['win'] == 256 * 1024
            h.tcp(4001, echo_port, 1, 1, ACK)
            h.tcp(4001, echo_port, 1, 1, ACK, b'hello')
            got, ack, seq = b'', None, 1
            while len(got) < 5 or ack != 6:
                r = h.segment(); assert r['flags'] == ACK, r
                if r['data']:
                    assert r['seq'] == seq; seq += len(r['data']); got += r['data']
                ack = r['ack']
            assert got == b'hello'
            # 600 KiB through a 256 KiB device window and a 32 KiB host window:
            # in-order, never past either window, every byte echoed.
            blob = bytes((i * 7) % 251 for i in range(600 * 1024))
            sent, host_seq, dev_ack, got, dev_win, acked = 0, 6, 6, b'', 256 * 1024, seq
            while len(got) < len(blob):
                while sent < len(blob) and host_seq - dev_ack < dev_win:
                    n = min(40000, len(blob) - sent, dev_win - (host_seq - dev_ack))
                    h.tcp(4001, echo_port, host_seq, seq, ACK, blob[sent:sent + n], win=32 * 1024 >> 8)
                    host_seq += n; sent += n
                r = h.segment(); assert r['flags'] == ACK, r
                dev_ack, dev_win = r['ack'], r['win']
                if r['data']:
                    assert r['seq'] == seq and len(r['data']) <= 16384 - 36
                    assert r['seq'] + len(r['data']) - acked <= 32 * 1024   # host window respected
                    seq += len(r['data']); got += r['data']
                    # ACK at most 32 KiB behind, the window the host advertised.
                    h.tcp(4001, echo_port, host_seq, seq, ACK, win=32 * 1024 >> 8); acked = seq
            assert got == blob[:len(got)] and len(got) == len(blob)
            # Host RST closes the local socket (echo thread sees EOF).
            h.tcp(4001, echo_port, host_seq, seq, RST)
            # Server closes after a banner -> data then RST from the device.
            h.tcp(4002, banner_port, 0, 0, SYN); assert h.segment()['flags'] == SYN | ACK
            h.tcp(4002, banner_port, 1, 1, ACK)
            data = b''
            while True:
                r = h.segment()
                if r['flags'] & RST: break
                data += r['data']
            assert data == b'lockdown-banner' and closed.wait(2)
            # Data for an unknown connection is answered with RST.
            h.tcp(4003, echo_port, 9, 9, ACK, b'x'); assert h.segment()['flags'] & RST
            # A new VERSION resets the session and restarts device sequence numbers.
            h.tcp(4004, echo_port, 0, 0, SYN); assert h.segment()['flags'] == SYN | ACK
            h.s.sendall(struct.pack('>IIIII', 0, 20, 2, 0, 0)); assert h.packet()[0] == 0
            h.expect = 0
            h.tcp(4004, echo_port, 1, 1, ACK, b'stale'); assert h.segment()['flags'] & RST
        finally:
            host_end.shutdown(socket.SHUT_RDWR); host_end.close()
            _, err = daemon.communicate(timeout=10)
        assert daemon.returncode == 0, (daemon.returncode, err)
        for line in ('version: host 2 features 0x8 -> device 2', f'connect {echo_port} <- host 4001',
                     f'connect {dead_port} <- host 4000 refused', 'session reset: version'):
            assert line in err, (line, err)
        print('PASS: usbmux daemon: version 2 handshake, SETUP ignored, refused port RST, SYN/ACK, '
              '600 KiB echo within device 256 KiB and host 32 KiB windows, host RST, server close RST, '
              'unknown-connection RST, VERSION resets sequence numbers')


main()

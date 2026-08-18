#!/usr/bin/env python3
"""Host lab client for the production M4B3 TCP receiver.

Usage:
  python3 firmware/scripts/m4b3_lab_client.py --host 192.168.x.x hello-key
  python3 firmware/scripts/m4b3_lab_client.py --host 192.168.x.x nack
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import zlib

MAGIC = b"M4B3"
WIDTH, HEIGHT, STRIDE, KEY_SIZE = 480, 800, 60, 48000
HELLO_H, KEY_H, PATCH_H, ACK_H, PING_H = 17, 19, 14, 9, 4
TYPE_HELLO, TYPE_KEY, TYPE_PATCH, TYPE_ACK, TYPE_PING, TYPE_PONG = 1, 2, 3, 4, 5, 6
ACK_OK, NACK_CRC, NACK_BASE = 0, 1, 2
MAX_PAYLOAD = 96 * 1024


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def wrap(typ: int, header: bytes, payload: bytes = b"", seq: int = 0) -> bytes:
    return MAGIC + struct.pack("<BBHII", typ, 0, len(header), len(payload), seq) + header + payload


def encode_hello(seq: int = 0, status: int = 0) -> bytes:
    header = bytes([1]) + struct.pack("<HH", WIDTH, HEIGHT) + bytes([1])
    header += struct.pack("<HII", STRIDE, 3, MAX_PAYLOAD) + bytes([status])
    return wrap(TYPE_HELLO, header, b"", seq)


def encode_key(frame_id: int, fb: bytes, seq: int = 1) -> bytes:
    header = struct.pack("<IHHBHI", frame_id, WIDTH, HEIGHT, 1, STRIDE, KEY_SIZE) + struct.pack("<I", crc32(fb))
    return wrap(TYPE_KEY, header, fb, seq)


def encode_patch(frame_id: int, base_id: int, fb: bytes, x: int, y: int, w: int, h: int, seq: int = 2,
                 corrupt_crc: bool = False) -> bytes:
    stride = w // 8
    payload = bytearray()
    payload += struct.pack("<HHHHI", x, y, w, h, stride * h)
    for row in range(h):
        off = (y + row) * STRIDE + (x // 8)
        payload += fb[off:off + stride]
    crc = crc32(fb)
    if corrupt_crc:
        crc ^= 0xFFFFFFFF
    header = struct.pack("<IIHI", frame_id, base_id, 1, crc)
    return wrap(TYPE_PATCH, header, bytes(payload), seq)


def read_msg(sock: socket.socket, timeout: float = 5.0) -> bytes:
    sock.settimeout(timeout)
    hdr = b""
    while len(hdr) < 16:
        chunk = sock.recv(16 - len(hdr))
        if not chunk:
            raise EOFError("eof on envelope")
        hdr += chunk
    if hdr[:4] != MAGIC:
        raise ValueError(f"bad magic {hdr[:4]!r}")
    _typ, _flags, hlen, plen, _seq = struct.unpack_from("<BBHII", hdr, 4)
    if hlen > 32 or plen > MAX_PAYLOAD:
        raise ValueError(f"oversized h={hlen} p={plen}")
    body = b""
    need = hlen + plen
    while len(body) < need:
        chunk = sock.recv(need - len(body))
        if not chunk:
            raise EOFError("eof on body")
        body += chunk
    return hdr + body


def parse_ack(msg: bytes) -> tuple[int, int, int]:
    typ = msg[4]
    if typ != TYPE_ACK:
        return typ, -1, -1
    result = msg[20]
    accepted_s = struct.unpack_from("<i", msg, 21)[0]
    return typ, result, accepted_s


def white() -> bytearray:
    return bytearray(b"\xff" * KEY_SIZE)


def set_black(fb: bytearray, x: int, y: int) -> None:
    off = y * STRIDE + (x >> 3)
    fb[off] = fb[off] & ~(0x80 >> (x & 7))


def connect(host: str, port: int) -> socket.socket:
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def cmd_hello_key(host: str, port: int) -> int:
    fb = white()
    with connect(host, port) as sock:
        sock.sendall(encode_hello())
        hello = read_msg(sock)
        if hello[4] != TYPE_HELLO or hello[16 + 16] != 0:
            print("HELLO failed", file=sys.stderr)
            return 2
        sock.sendall(encode_key(0, bytes(fb)))
        ack = read_msg(sock)
        typ, result, accepted = parse_ack(ack)
        print(f"hello=ok key_ack result={result} accepted={accepted} crc=0x{crc32(bytes(fb)):08X}")
        return 0 if typ == TYPE_ACK and result == ACK_OK and accepted == 0 else 2


def cmd_nack(host: str, port: int) -> int:
    fb = white()
    with connect(host, port) as sock:
        sock.sendall(encode_hello())
        read_msg(sock)
        sock.sendall(encode_key(0, bytes(fb)))
        typ, result, accepted = parse_ack(read_msg(sock))
        if result != ACK_OK:
            print("key not accepted", file=sys.stderr)
            return 2
        before = accepted
        before_crc = crc32(bytes(fb))
        rogue = white()
        set_black(rogue, 16, 16)
        sock.sendall(encode_patch(9, 99, bytes(rogue), 16, 16, 16, 16, seq=3))
        typ, result, accepted = parse_ack(read_msg(sock))
        print(f"wrong_base result={result} accepted={accepted} before={before}")
        if result != NACK_BASE or accepted != before:
            return 2
        flipped = white()
        set_black(flipped, 32, 32)
        sock.sendall(encode_patch(2, 0, bytes(flipped), 32, 32, 16, 16, seq=4, corrupt_crc=True))
        typ, result, accepted = parse_ack(read_msg(sock))
        print(f"bad_crc result={result} accepted={accepted} crc=0x{before_crc:08X}")
        return 0 if result == NACK_CRC and accepted == before else 2


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--port", type=int, default=48624)
    p.add_argument("cmd", choices=("hello-key", "nack"))
    args = p.parse_args()
    if args.cmd == "hello-key":
        return cmd_hello_key(args.host, args.port)
    return cmd_nack(args.host, args.port)


if __name__ == "__main__":
    raise SystemExit(main())

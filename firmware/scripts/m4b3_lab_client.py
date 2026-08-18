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
import time
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


def paint_rect(fb: bytearray, x: int, y: int, w: int, h: int) -> None:
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            set_black(fb, xx, yy)


def landmark_fb() -> bytearray:
    """Same asymmetric blocks as Android VirtualBrowserSession.landmarkHtml()."""
    fb = white()
    paint_rect(fb, 0, 0, 64, 64)        # TL
    paint_rect(fb, 448, 0, 32, 64)      # TR
    paint_rect(fb, 0, 768, 64, 32)      # BL
    paint_rect(fb, 432, 752, 48, 48)    # BR
    paint_rect(fb, 40, 80, 80, 40)      # A
    paint_rect(fb, 200, 200, 40, 80)    # B
    paint_rect(fb, 80, 500, 120, 24)    # C
    return fb


def sequence_fb(n: int) -> bytearray:
    fb = white()
    paint_rect(fb, 0, 0, 16, 16)
    paint_rect(fb, 20, 40 + (n % 8) * 80, 440, 48)
    paint_rect(fb, 40 + n * 40, 700, 32, 32)
    return fb


def hello_ok(sock: socket.socket) -> None:
    sock.sendall(encode_hello())
    hello = read_msg(sock)
    if hello[4] != TYPE_HELLO or hello[16 + 16] != 0:
        raise RuntimeError("HELLO failed")


def send_key(sock: socket.socket, frame_id: int, fb: bytes, seq: int) -> tuple[int, int, float]:
    t0 = time.perf_counter()
    sock.sendall(encode_key(frame_id, fb, seq=seq))
    ack = read_msg(sock, timeout=8.0)
    dt = (time.perf_counter() - t0) * 1000.0
    typ, result, accepted = parse_ack(ack)
    if typ != TYPE_ACK or result != ACK_OK:
        raise RuntimeError(f"KEY NACK result={result} accepted={accepted}")
    return result, accepted, dt


def encode_ping(seq: int = 10, nonce: int = 1) -> bytes:
    return wrap(TYPE_PING, struct.pack("<I", nonce), b"", seq)


def cmd_landmark(host: str, port: int) -> int:
    fb = bytes(landmark_fb())
    with connect(host, port) as sock:
        hello_ok(sock)
        result, accepted, dt = send_key(sock, 0, fb, 1)
        print(f"landmark ack result={result} accepted={accepted} ack_ms={dt:.1f} crc=0x{crc32(fb):08X}")
        return 0 if accepted == 0 else 2


def cmd_sequence(host: str, port: int, count: int, interval: float) -> int:
    with connect(host, port) as sock:
        hello_ok(sock)
        last_crc = 0
        for i in range(count):
            fb = bytes(sequence_fb(i))
            last_crc = crc32(fb)
            result, accepted, dt = send_key(sock, i, fb, i + 1)
            print(f"seq={i} ack result={result} accepted={accepted} ack_ms={dt:.1f} crc=0x{last_crc:08X}")
            if accepted != i:
                return 2
            if i + 1 < count:
                time.sleep(interval)
        print(f"sequence ok count={count} last_crc=0x{last_crc:08X}")
        return 0


def cmd_burst(host: str, port: int, count: int) -> int:
    with connect(host, port) as sock:
        hello_ok(sock)
        t0 = time.perf_counter()
        last_crc = 0
        for i in range(count):
            fb = bytes(sequence_fb(i))
            last_crc = crc32(fb)
            result, accepted, dt = send_key(sock, i, fb, i + 1)
            print(f"burst={i} ack result={result} accepted={accepted} ack_ms={dt:.1f} crc=0x{last_crc:08X}")
            if accepted != i:
                return 2
        elapsed = (time.perf_counter() - t0) * 1000.0
        print(f"burst ok count={count} elapsed_ms={elapsed:.1f} last_crc=0x{last_crc:08X}")
        return 0


def cmd_ping_hold(host: str, port: int) -> int:
    fb = bytes(landmark_fb())
    with connect(host, port) as sock:
        hello_ok(sock)
        result, accepted, dt = send_key(sock, 0, fb, 1)
        print(f"key ack result={result} accepted={accepted} ack_ms={dt:.1f}")
        for i in range(5):
            nonce = 0x1000 + i
            sock.sendall(encode_ping(seq=20 + i, nonce=nonce))
            pong = read_msg(sock, timeout=3.0)
            if pong[4] != TYPE_PONG:
                print(f"expected PONG got type={pong[4]}", file=sys.stderr)
                return 2
            got = struct.unpack_from("<I", pong, 16)[0]
            print(f"pong nonce=0x{got:08X}")
            if got != nonce:
                return 2
            time.sleep(0.2)
        print(f"ping-hold ok accepted={accepted} crc=0x{crc32(fb):08X}")
        return 0


def sparse_fb(n: int) -> bytearray:
    """Landmark baseline + one small walking block (byte-boundary x)."""
    fb = landmark_fb()
    # Keep the extra block away from the landmark stamps so dirty stays 1-2 windows.
    x = 11 + (n * 17) % 200
    y = 300 + (n * 13) % 160
    paint_rect(fb, x, y, 24, 16)
    return fb


def two_region_fb(n: int) -> bytearray:
    fb = landmark_fb()
    paint_rect(fb, 120 + (n % 4) * 8, 320, 16, 16)
    paint_rect(fb, 280, 360 + (n % 3) * 8, 16, 16)
    return fb


def dense_fb() -> bytearray:
    fb = landmark_fb()
    paint_rect(fb, 0, 80, 480, 360)  # large mid-band overwrite, >28% physical
    return fb


def fragmented_fb() -> bytearray:
    fb = landmark_fb()
    paint_rect(fb, 96, 120, 8, 8)
    paint_rect(fb, 240, 160, 8, 8)
    paint_rect(fb, 360, 280, 8, 8)
    paint_rect(fb, 140, 640, 8, 8)
    paint_rect(fb, 300, 680, 8, 8)
    return fb


def cmd_sparse(host: str, port: int, count: int, interval: float, start_id: int) -> int:
    with connect(host, port) as sock:
        hello_ok(sock)
        last_crc = 0
        for i in range(count):
            fb = bytes(sparse_fb(i))
            last_crc = crc32(fb)
            fid = start_id + i
            result, accepted, dt = send_key(sock, fid, fb, fid + 1)
            print(f"sparse={i} id={fid} ack result={result} accepted={accepted} "
                  f"ack_ms={dt:.1f} crc=0x{last_crc:08X}")
            if accepted != fid:
                return 2
            if i + 1 < count:
                time.sleep(interval)
        print(f"sparse ok count={count} last_crc=0x{last_crc:08X}")
        return 0


def cmd_two_region(host: str, port: int, count: int, interval: float, start_id: int) -> int:
    with connect(host, port) as sock:
        hello_ok(sock)
        last_crc = 0
        for i in range(count):
            fb = bytes(two_region_fb(i))
            last_crc = crc32(fb)
            fid = start_id + i
            result, accepted, dt = send_key(sock, fid, fb, fid + 1)
            print(f"two={i} id={fid} ack result={result} accepted={accepted} "
                  f"ack_ms={dt:.1f} crc=0x{last_crc:08X}")
            if accepted != fid:
                return 2
            if i + 1 < count:
                time.sleep(interval)
        print(f"two-region ok count={count} last_crc=0x{last_crc:08X}")
        return 0


def cmd_dense(host: str, port: int, start_id: int) -> int:
    fb = bytes(dense_fb())
    with connect(host, port) as sock:
        hello_ok(sock)
        result, accepted, dt = send_key(sock, start_id, fb, start_id + 1)
        print(f"dense id={start_id} ack result={result} accepted={accepted} "
              f"ack_ms={dt:.1f} crc=0x{crc32(fb):08X}")
        return 0 if accepted == start_id else 2


def cmd_fragmented(host: str, port: int, start_id: int) -> int:
    fb = bytes(fragmented_fb())
    with connect(host, port) as sock:
        hello_ok(sock)
        result, accepted, dt = send_key(sock, start_id, fb, start_id + 1)
        print(f"frag id={start_id} ack result={result} accepted={accepted} "
              f"ack_ms={dt:.1f} crc=0x{crc32(fb):08X}")
        return 0 if accepted == start_id else 2


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
    p.add_argument("--count", type=int, default=6)
    p.add_argument("--interval", type=float, default=2.5)
    p.add_argument("--start-id", type=int, default=0)
    p.add_argument(
        "cmd",
        choices=(
            "hello-key",
            "nack",
            "landmark",
            "sequence",
            "burst",
            "ping-hold",
            "sparse",
            "two-region",
            "dense",
            "fragmented",
        ),
    )
    args = p.parse_args()
    if args.cmd == "hello-key":
        return cmd_hello_key(args.host, args.port)
    if args.cmd == "landmark":
        return cmd_landmark(args.host, args.port)
    if args.cmd == "sequence":
        return cmd_sequence(args.host, args.port, args.count, args.interval)
    if args.cmd == "burst":
        return cmd_burst(args.host, args.port, args.count)
    if args.cmd == "ping-hold":
        return cmd_ping_hold(args.host, args.port)
    if args.cmd == "sparse":
        return cmd_sparse(args.host, args.port, args.count, args.interval, args.start_id)
    if args.cmd == "two-region":
        return cmd_two_region(args.host, args.port, args.count, args.interval, args.start_id)
    if args.cmd == "dense":
        return cmd_dense(args.host, args.port, args.start_id)
    if args.cmd == "fragmented":
        return cmd_fragmented(args.host, args.port, args.start_id)
    return cmd_nack(args.host, args.port)


if __name__ == "__main__":
    raise SystemExit(main())

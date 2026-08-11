#!/usr/bin/env python3
"""M4 Waveform Lab — USB LUT experimentation tool (Murphy M4).

Commands (all go through the m4adb daemon socket, never a raw serial open):
  lut_begin {slot:0|1, size:48000}   start frame upload (chunks follow)
  lut_upload {lut: b64 110B}         set experiment LUT (voltages locked by default)
  lut_run {swap:bool}                refresh prev(0)->next(1) with current LUT
  lut_swap                            swap slots
  lut_stats                           {last_ms, runs, lut_set, frames_ready}
  lut_clear                           safe full-refresh recovery
  lut_end                             leave experiment mode

Usage examples:
  # upload frame A (slot 0) then frame B (slot 1) from 1bpp PBM files
  python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 frame0 a.pbm
  python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 frame1 b.pbm
  # build a LUT (105 waveform bytes + locked voltage tail) and run
  python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 lut --hex 00,00,...
  python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 run
  python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 stats
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from m4adb_lib.client import Client  # noqa: E402
from m4adb_lib.daemon import DaemonTransport, socket_path_for_port  # noqa: E402

FRAME_BYTES = 48000
CHUNK = 512
LUT_BYTES = 110
SAFE_VOLTAGE_TAIL = bytes([0x17, 0x41, 0xA8, 0x32, 0x30])


def open_client(port: str) -> Client:
    # Reuse the single m4adb daemon socket; never open the USB port directly.
    c = Client(DaemonTransport(socket_path_for_port(port), timeout=3.0), default_timeout=10)
    c.wait_ready(timeout=30)
    return c


def upload_frame(client: Client, slot: int, pbm_path: Path) -> None:
    raw = pbm_path.read_bytes()
    # Accept both raw PBM (P4 header) and bare 48000-byte payload.
    if len(raw) == FRAME_BYTES:
        payload = raw
    else:
        # P4 PBM: skip magic + dimensions line(s) up to the first blank or
        # the final newline before the raster (header ends after "W H\n").
        if not raw.startswith(b"P4"):
            raise SystemExit("not a P4 PBM and not a bare 48000-byte frame")
        idx = 2
        lines = 0
        while idx < len(raw) and lines < 2:
            nl = raw.find(b"\n", idx)
            if nl < 0:
                break
            idx = nl + 1
            lines += 1
        payload = raw[idx:]
        if len(payload) != FRAME_BYTES:
            raise SystemExit(f"frame must be {FRAME_BYTES} bytes, got {len(payload)}")
    r = client.request({"op": "lut_begin", "slot": slot, "size": FRAME_BYTES}, timeout=10)
    if not r.get("ready"):
        raise SystemExit(f"lut_begin failed: {r}")
    total = (FRAME_BYTES + CHUNK - 1) // CHUNK
    for seq in range(total):
        part = payload[seq * CHUNK: (seq + 1) * CHUNK]
        resp = client.send_raw_chunk(seq, total, part, timeout=15)
        if "chunk" not in resp and not resp.get("ok"):
            raise SystemExit(f"chunk {seq} failed: {resp}")
    time.sleep(0.2)
    print(f"frame slot {slot}: {FRAME_BYTES} bytes uploaded")


def upload_lut(client: Client, hex_bytes: bytes, unlock: bool) -> None:
    if len(hex_bytes) < 105:
        raise SystemExit("LUT must be >= 105 bytes")
    lut = bytearray(hex_bytes[:LUT_BYTES])
    if not unlock:
        lut[105:] = SAFE_VOLTAGE_TAIL
    b64 = base64.b64encode(bytes(lut)).decode()
    # The device CDC path occasionally drops a long request line while the
    # e-ink loop is busy; retry a few times before giving up.
    last = None
    for attempt in range(5):
        try:
            r = client.request({"op": "lut_upload", "lut": b64, "unlock_voltages": unlock}, timeout=12)
            if r.get("ok"):
                print(f"LUT set ({len(lut)} bytes, voltages {'unlocked' if unlock else 'LOCKED safe tail'})")
                return
            last = r
        except Exception as e:  # noqa: BLE001
            last = e
        time.sleep(1.0)
    raise SystemExit(f"lut_upload failed after retries: {last}")


def animate(client: Client, args) -> None:
    """Walk SD frames frame_000..frame_NNN-1, refreshing each transition."""
    dirp = args.dir.rstrip("/")
    frames = []
    for i in range(args.count):
        frames.append(f"{dirp}/frame_{i:03d}.bin")
    # Establish the physical baseline first: FULL-refresh the panel to frame 0
    # so every subsequent FAST differential run starts from a known state.
    r = client.request({"op": "lut_baseline", "frame": frames[0]}, timeout=120)
    if not r.get("ok"):
        raise SystemExit(f"baseline {frames[0]} failed: {r}")
    print(f"baseline: {frames[0]} (FULL)")
    seq = list(range(1, args.count))
    if args.swap:
        seq = seq + list(range(args.count - 2, 0, -1))
    total_ms = 0
    for i in seq:
        prev = frames[i - 1]
        nxt = frames[i]
        r = client.request({"op": "lut_set_frames", "prev": prev, "next": nxt}, timeout=10)
        if not r.get("ok"):
            raise SystemExit(f"set_frames failed at {prev}->{nxt}: {r}")
        r = client.request({"op": "lut_run", "swap": False}, timeout=60)
        ms = r.get("ms", 0)
        total_ms += ms
        print(f"{i:3d}: {prev} -> {nxt}  {ms} ms")
        if args.delay > 0:
            time.sleep(args.delay)
    print(f"animate done: {len(seq)} steps, {total_ms} ms total")


def gen_frames(args) -> None:
    """Generate a page-turn intermediate frame sequence (stdlib only).

    Frame content: a text-like page (black lines) that is revealed left-to-right
    as a page edge sweeps across (kindle-style turn).  Frame 0 = mostly black,
    frame N-1 = mostly white; intermediates slide the content edge.
    """
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    W, H = 800, 480
    wb = W // 8
    for i in range(args.count):
        t = i / max(1, args.count - 1)
        # Revealed content width grows with t; a black edge band sweeps right.
        reveal_w = int(W * 0.8 * t)
        edge = int(W * t)
        raw = bytearray(FRAME_BYTES)
        # Content region: black horizontal text lines (revealed portion).
        for row in range(0, H, 10):
            y_off = row * wb
            x_end = min(reveal_w, W)
            for byte_x in range(0, x_end, 8):
                if byte_x + 8 <= x_end:
                    raw[y_off + byte_x // 8] = 0x00  # solid black line chunk
        # Page edge band: 6 columns of black sweeping left->right.
        for row in range(0, H, 2):
            y_off = row * wb
            for dx in range(0, 6):
                x = edge + dx
                if 0 <= x < W:
                    raw[y_off + x // 8] |= (0x80 >> (x % 8))
        (out / f"frame_{i:03d}.bin").write_bytes(bytes(raw))
        if args.reverse and 0 < i < args.count - 1:
            (out / f"frame_r_{args.count - 1 - i:03d}.bin").write_bytes(bytes(raw))
    print(f"generated {args.count} frames in {out}")


def push_frames(args) -> None:
    """Push generated frames to the device SD via the file-transfer HTTP."""
    import urllib.request
    import uuid as _uuid

    device = args.push
    if device.startswith("/"):
        # Device-side path (e.g. /waveform): use m4adb wifi_transfer to bring
        # the file-transfer UI up, then POST each frame to /upload.
        ip = None
        # The tool holds a daemon client; ask the user for the device IP once.
        ip = args.device_ip or ""
        if not ip:
            raise SystemExit("--push needs --device-ip (device HTTP IP) or a local dir")
        base = f"http://{ip}"
        for p in sorted(Path(args.out).glob("frame_*.bin")):
            boundary = _uuid.uuid4().hex
            body = (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="file"; filename="{p.name}"\r\n'
                "Content-Type: application/octet-stream\r\n\r\n"
            ).encode() + p.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
            req = urllib.request.Request(
                f"{base}/upload?path={device}",
                data=body,
                method="POST",
                headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
            )
            with urllib.request.urlopen(req, timeout=60) as r:
                r.read()
            print(f"pushed {p.name}")
    else:
        # Local dir: copy into the given local directory.
        dst = Path(args.push)
        dst.mkdir(parents=True, exist_ok=True)
        for p in sorted(Path(args.out).glob("frame_*.bin")):
            (dst / p.name).write_bytes(p.read_bytes())
        print(f"copied frames to {dst}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbmodem101")
    ap.add_argument("--device-ip", default="", help="device HTTP IP for --push (e.g. 192.168.0.138)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("frame0"); p.add_argument("pbm"); p.set_defaults(fn=None)
    p = sub.add_parser("frame1"); p.add_argument("pbm"); p.set_defaults(fn=None)
    p = sub.add_parser("sdframes"); p.add_argument("prev"); p.add_argument("next")
    p = sub.add_parser("baseline"); p.add_argument("frame")
    p = sub.add_parser("wipe")
    p.add_argument("prev")
    p.add_argument("next")
    p.add_argument("--steps", type=int, default=6)
    p.add_argument("--feather", type=int, default=0)
    p.add_argument("--tail", type=int, default=0, help="ghost-clear tail ms (0=off)")
    p.add_argument("--dir", type=int, default=0,
                   help="0=right->left, 1=left->right, 2=bottom->top, 3=top->bottom")
    p = sub.add_parser("wipewin")
    p.add_argument("prev")
    p.add_argument("next")
    p.add_argument("--steps", type=int, default=8,
                   help="advance count (= refresh count; default 8)")
    p.add_argument("--tail", type=int, default=0, help="ghost-clear tail ms (0=off)")
    p.add_argument("--mult", type=int, default=1,
                   help="window width in step units (default 1; e.g. 8 = 8 steps wide)")
    p.add_argument("--dir", type=int, default=0,
                   help="0=R→L 1=L→R 2=B→T 3=T→B")
    p = sub.add_parser("settle")
    p.add_argument("prev")
    p.add_argument("next")
    p = sub.add_parser("lut")
    p.add_argument("--hex", help="comma/space separated 105+ bytes (waveform first)")
    p.add_argument("--unlock", action="store_true", help="allow voltage bytes to pass through")
    p = sub.add_parser("run"); p.add_argument("--swap", action="store_true")
    p = sub.add_parser("animate")
    p.add_argument("--dir", default="/waveform", help="SD dir with frame_000.bin..frame_NNN.bin")
    p.add_argument("--count", type=int, required=True, help="number of frames to animate")
    p.add_argument("--delay", type=float, default=0.0, help="pause between frames (s)")
    p.add_argument("--swap", action="store_true", help="run frames in reverse after forward pass")
    p = sub.add_parser("gen")
    p.add_argument("--out", default="waveform_frames", help="local output dir (PNG/PBM)")
    p.add_argument("--count", type=int, default=16, help="number of intermediate frames")
    p.add_argument("--reverse", action="store_true", help="also emit reversed frames")
    p.add_argument("--push", help="device path to upload frames to (e.g. /waveform)")
    p = sub.add_parser("swap")
    p = sub.add_parser("stats")
    p = sub.add_parser("clear")
    p = sub.add_parser("end")
    args = ap.parse_args()

    if args.cmd == "gen":
        gen_frames(args)
        if args.push:
            push_frames(args)
        return

    c = open_client(args.port)
    if args.cmd == "lut":
        if not args.hex:
            raise SystemExit("--hex required")
        data = bytes(int(x, 16) for x in args.hex.replace(",", " ").split())
        upload_lut(c, data, args.unlock)
    elif args.cmd == "frame0":
        upload_frame(c, 0, Path(args.pbm))
    elif args.cmd == "frame1":
        upload_frame(c, 1, Path(args.pbm))
    elif args.cmd == "sdframes":
        r = c.request({"op": "lut_set_frames", "prev": args.prev, "next": args.next}, timeout=10)
        print(json.dumps(r, ensure_ascii=False))
    elif args.cmd == "baseline":
        r = c.request({"op": "lut_baseline", "frame": args.frame}, timeout=60)
        print(json.dumps(r, ensure_ascii=False))
    elif args.cmd == "animate":
        animate(c, args)
    elif args.cmd == "wipe":
        r = c.request({"op": "lut_animate", "prev": args.prev, "next": args.next,
                       "steps": args.steps, "feather": args.feather,
                       "tail_ms": args.tail, "dir": args.dir}, timeout=120)
        print(json.dumps(r, ensure_ascii=False))
    elif args.cmd == "wipewin":
        r = c.request({"op": "lut_wipe", "prev": args.prev, "next": args.next,
                       "steps": args.steps, "tail_ms": args.tail, "win_mult": args.mult,
                       "dir": args.dir}, timeout=120)
        print(json.dumps(r, ensure_ascii=False))
    elif args.cmd == "settle":
        r = c.request({"op": "lut_settle", "prev": args.prev, "next": args.next}, timeout=120)
        print(json.dumps(r, ensure_ascii=False))
    elif args.cmd == "touch":
        if args.off:
            r = c.request({"op": "lut_touch", "on": False}, timeout=15)
        else:
            r = c.request({"op": "lut_touch", "on": True, "page_a": args.page_a,
                           "page_b": args.page_b, "steps": args.steps,
                           "tail_ms": args.tail}, timeout=15)
        print(json.dumps(r, ensure_ascii=False))
    elif args.cmd == "run":
        r = c.request({"op": "lut_run", "swap": args.swap}, timeout=30)
        print(json.dumps(r, ensure_ascii=False))
    elif args.cmd == "swap":
        print(json.dumps(c.request({"op": "lut_swap"}, timeout=10), ensure_ascii=False))
    elif args.cmd == "stats":
        print(json.dumps(c.request({"op": "lut_stats"}, timeout=10), ensure_ascii=False))
    elif args.cmd == "clear":
        print(json.dumps(c.request({"op": "lut_clear"}, timeout=60), ensure_ascii=False))
    elif args.cmd == "end":
        print(json.dumps(c.request({"op": "lut_end"}, timeout=10), ensure_ascii=False))
    c.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Boot a Murphy M4 16 MiB flash image with Espressif ESP32-S3 QEMU directly.

Unlike ``run_full_flash.py`` this launcher does **not** require a full ESP-IDF
``idf.py`` environment. It shells out to ``qemu-system-xtensa`` with the flags
needed for Murphy-sized images:

  * 16 MiB flash image (``if=mtd,format=raw``)
  * ``-m 8M`` (or 16/32) PSRAM size
  * SPI flash boot strap ``strap_mode=0x04``
  * TG WDT disabled via ``timer.esp32c3.timg`` (Espressif docs: S3 reuses C3 name)

**Production** Murphy firmware (N16R8 / octal PSRAM) currently hangs in
Espressif QEMU 9.2.2 when ``ssi_psram is_octal=true`` while loading a valid
~5 MiB app; without ``is_octal`` the OPI driver fails. Prefer the QEMU-only
PlatformIO env ``murphy_m4_qemu`` (``qio_qspi`` = Quad PSRAM) and run
**without** ``--octal``. That profile is **not** for real hardware.

Example::

  pio run -e murphy_m4_qemu

  python3 tools/murphy_flash_image.py \\
    --build-dir ../wap-checkpoint/firmware/.pio/build/murphy_m4_qemu \\
    -o /tmp/murphy-qemu.bin

  python3 qemu/run_murphy_bin.py /tmp/murphy-qemu.bin --seconds 40 \\
    --serial-file /tmp/murphy-qemu-serial.log \\
    --screen-file /tmp/murphy-qemu-screen.pbm --probe
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

FLASH_SIZE = 16 * 1024 * 1024
_DEFAULT_QEMU_CANDIDATES = (
    "qemu-system-xtensa",
    str(
        Path.home()
        / ".espressif/tools/qemu-xtensa/esp_develop_9.2.2_20250817/qemu/bin/qemu-system-xtensa"
    ),
)


class RunnerError(RuntimeError):
    pass


_FRAME_PREFIX = "[M4-QEMU-FB]"


def extract_last_frame(log_text: str) -> tuple[int, int, bytes]:
    """Return the last complete framebuffer emitted by the QEMU display shim."""
    current: tuple[int, int, int, list[str]] | None = None
    last: tuple[int, int, bytes] | None = None
    for line in log_text.splitlines():
        if line.startswith(f"{_FRAME_PREFIX} BEGIN "):
            fields = line.split()
            try:
                width, height, size = map(int, fields[-3:])
            except (ValueError, IndexError):
                current = None
                continue
            current = (width, height, size, [])
        elif current is not None and line.startswith(f"{_FRAME_PREFIX} D "):
            current[3].append(line.removeprefix(f"{_FRAME_PREFIX} D ").strip())
        elif current is not None and line == f"{_FRAME_PREFIX} END":
            width, height, size, chunks = current
            try:
                payload = bytes.fromhex("".join(chunks))
            except ValueError:
                current = None
                continue
            if width > 0 and height > 0 and size == len(payload) == width * height // 8:
                last = (width, height, payload)
            current = None
    if last is None:
        raise RunnerError("no complete [M4-QEMU-FB] frame found in serial log")
    return last


def write_portrait_pbm(path: Path, width: int, height: int, frame: bytes) -> None:
    """Rotate the physical landscape frame CCW and write a viewer-friendly PBM."""
    if width % 8 or height % 8 or len(frame) != width * height // 8:
        raise RunnerError("invalid 1bpp framebuffer geometry")
    physical_row_bytes = width // 8
    output_row_bytes = height // 8
    rotated = bytearray(output_row_bytes * width)
    for out_y in range(width):
        for out_x in range(height):
            in_x = out_y
            in_y = height - 1 - out_x
            is_white = (frame[in_y * physical_row_bytes + in_x // 8] >> (7 - in_x % 8)) & 1
            if not is_white:
                rotated[out_y * output_row_bytes + out_x // 8] |= 1 << (7 - out_x % 8)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(f"P4\n{height} {width}\n".encode("ascii") + rotated)


def find_qemu(explicit: str | None) -> str:
    if explicit:
        path = Path(explicit).expanduser()
        if not path.is_file():
            raise RunnerError(f"qemu binary not found: {path}")
        return str(path.resolve())
    env = os.environ.get("QEMU_XTENSA")
    if env and Path(env).is_file():
        return env
    for cand in _DEFAULT_QEMU_CANDIDATES:
        found = shutil.which(cand) if "/" not in cand else (cand if Path(cand).is_file() else None)
        if found:
            return found
    # Last chance: scan espressif tools tree.
    root = Path.home() / ".espressif/tools/qemu-xtensa"
    if root.is_dir():
        for p in sorted(root.rglob("qemu-system-xtensa")):
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)
    raise RunnerError(
        "qemu-system-xtensa not found. Install with: "
        "python3 $IDF_PATH/tools/idf_tools.py install qemu-xtensa "
        "(and brew install libgcrypt on macOS)"
    )


def build_cmd(
    qemu: str,
    flash: Path,
    *,
    psram_mb: int,
    octal: bool,
    serial_file: Path | None,
    gdb: bool,
    extra: list[str],
) -> list[str]:
    cmd = [
        qemu,
        "-nographic",
        "-machine",
        "esp32s3",
        "-m",
        f"{psram_mb}M",
        "-drive",
        f"file={flash},if=mtd,format=raw",
        "-global",
        "driver=esp32s3.gpio,property=strap_mode,value=0x04",
        # Espressif QEMU docs: S3 still uses the C3 timer-group driver name.
        "-global",
        "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
        "-nic",
        "user,model=open_eth",
    ]
    if octal:
        cmd += ["-global", "driver=ssi_psram,property=is_octal,value=true"]
    if serial_file is not None:
        cmd += ["-serial", f"file:{serial_file}"]
    else:
        cmd += ["-serial", "mon:stdio"]
    if gdb:
        cmd += ["-gdb", "tcp::3333", "-S"]
    cmd += extra
    return cmd


def run_with_timeout(cmd: list[str], seconds: float, log_path: Path | None) -> int:
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE if log_path else None,
        stderr=subprocess.STDOUT if log_path else None,
        text=True,
        bufsize=1,
    )
    chunks: list[str] = []
    deadline = time.time() + seconds
    try:
        if log_path is None:
            # Interactive-ish: just wait.
            while time.time() < deadline and proc.poll() is None:
                time.sleep(0.2)
        else:
            assert proc.stdout is not None
            while time.time() < deadline:
                if proc.poll() is not None:
                    rest = proc.stdout.read() or ""
                    if rest:
                        chunks.append(rest)
                        sys.stdout.write(rest)
                    break
                line = proc.stdout.readline()
                if line:
                    chunks.append(line)
                    sys.stdout.write(line)
                    sys.stdout.flush()
                else:
                    time.sleep(0.02)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        if log_path is not None and chunks:
            # When -serial mon:stdio, stdout is the serial stream.
            # When -serial file:, serial is already on disk; still keep stdout copy.
            log_path.write_text("".join(chunks), encoding="utf-8", errors="replace")
    return proc.returncode if proc.returncode is not None else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flash_image", help="16 MiB raw flash image")
    parser.add_argument("--qemu", default=None, help="path to qemu-system-xtensa")
    parser.add_argument("--seconds", type=float, default=40.0, help="run duration")
    parser.add_argument("--psram-mb", type=int, default=8, choices=(2, 4, 8, 16, 32))
    parser.add_argument(
        "--octal",
        action="store_true",
        help="enable ssi_psram is_octal (production N16R8; hangs on QEMU 9.2.2 "
        "while loading a valid large app — prefer murphy_m4_qemu + Quad PSRAM)",
    )
    parser.add_argument(
        "--no-octal",
        action="store_true",
        help=argparse.SUPPRESS,  # legacy alias: default is already non-octal
    )
    parser.add_argument("--log", default=None, help="capture serial/stdout to this path")
    parser.add_argument(
        "--serial-file",
        default=None,
        help="also write guest UART to this file via -serial file:",
    )
    parser.add_argument(
        "--screen-file",
        default=None,
        help="extract the last QEMU framebuffer as a portrait PBM image",
    )
    parser.add_argument("--probe", action="store_true", help="run qemu/probe_boot.py on the log")
    parser.add_argument("--gdb", action="store_true", help="wait for GDB on :3333")
    parser.add_argument("--dry-run", action="store_true")
    # Do NOT use argparse.REMAINDER after a positional: it swallows all later
    # options (e.g. --seconds) and forwards them to QEMU as invalid flags.
    # Extra QEMU args go after a lone "--".
    args, unknown = parser.parse_known_args(argv)
    extra: list[str] = []
    if unknown:
        if unknown[0] == "--":
            extra = unknown[1:]
        elif "--" in unknown:
            idx = unknown.index("--")
            if idx != 0:
                print(
                    f"error: unrecognized arguments: {' '.join(unknown[:idx])}",
                    file=sys.stderr,
                )
                return 2
            extra = unknown[idx + 1 :]
        else:
            print(f"error: unrecognized arguments: {' '.join(unknown)}", file=sys.stderr)
            return 2

    flash = Path(args.flash_image).expanduser().resolve()
    if not flash.is_file():
        print(f"error: flash image not found: {flash}", file=sys.stderr)
        return 2
    if flash.stat().st_size != FLASH_SIZE:
        print(
            f"error: flash image must be exactly {FLASH_SIZE} bytes, got {flash.stat().st_size}",
            file=sys.stderr,
        )
        return 2

    try:
        qemu = find_qemu(args.qemu)
    except RunnerError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    screen_file = Path(args.screen_file).expanduser() if args.screen_file else None
    serial_file = Path(args.serial_file).expanduser() if args.serial_file else None
    log_path = Path(args.log).expanduser() if args.log else None
    if screen_file is not None and serial_file is None and log_path is None:
        serial_file = screen_file.with_suffix(screen_file.suffix + ".serial.log")
    # Prefer serial-file as the probe input when both are set.
    probe_src = serial_file or log_path

    cmd = build_cmd(
        qemu,
        flash,
        psram_mb=args.psram_mb,
        octal=bool(args.octal) and not bool(args.no_octal),
        serial_file=serial_file,
        gdb=args.gdb,
        extra=extra,
    )
    if args.dry_run:
        print("+", " ".join(cmd))
        return 0

    # When using -serial file:, capture QEMU process stderr/stdout separately.
    if serial_file is not None:
        print("+", " ".join(cmd), flush=True)
        with open(log_path or os.devnull, "w", encoding="utf-8") as out:
            proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT)
            try:
                time.sleep(args.seconds)
            finally:
                if proc.poll() is None:
                    proc.send_signal(signal.SIGTERM)
                    try:
                        proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=2)
        if serial_file.is_file():
            print(f"serial log: {serial_file} ({serial_file.stat().st_size} bytes)")
            # Print tail for convenience
            text = serial_file.read_text(encoding="utf-8", errors="replace")
            print(text[-4000:])
    else:
        run_with_timeout(cmd, args.seconds, log_path)

    if screen_file is not None:
        screen_src = serial_file or log_path
        assert screen_src is not None
        try:
            width, height, frame = extract_last_frame(
                screen_src.read_text(encoding="utf-8", errors="replace")
            )
            write_portrait_pbm(screen_file, width, height, frame)
        except (OSError, RunnerError) as exc:
            print(f"error: screen export failed: {exc}", file=sys.stderr)
            return 1
        print(f"screen: {screen_file} ({height}x{width} portrait PBM)")

    if args.probe and probe_src and probe_src.is_file():
        probe = Path(__file__).resolve().parent / "probe_boot.py"
        out_json = probe_src.with_suffix(probe_src.suffix + ".probe.json")
        print("+", "python3", probe, probe_src, "-o", out_json, flush=True)
        rc = subprocess.call([sys.executable, str(probe), str(probe_src), "-o", str(out_json)])
        if out_json.is_file():
            data = json.loads(out_json.read_text(encoding="utf-8"))
            acc = data.get("acceptance", {})
            print("probe summary:")
            print(f"  highest_stage={data.get('highest_stage')}")
            print(f"  failure_class={data.get('failure_class')}")
            for k, v in acc.items():
                print(f"  {k}={v}")
        return rc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# Clean-machine Murphy BIN simulation (`m4sim`)

Goal: after a fresh `git clone`, one entry boots the **same** patched QEMU chain
that already works on a developer machine that can simulate `firmware.bin`.

## Source of truth

See `simulator/qemu/LOCAL_RUNTIME_CHAIN.md`. Do **not** re-implement flash/QEMU
logic inside GitHub Actions; call `./m4sim` instead.

## Prerequisites

| Tool | Notes |
|------|--------|
| git, ninja, C/C++ toolchain | for `simulator/qemu/build.py` |
| glib, pixman, gcrypt, slirp (and often SDL) | QEMU host deps |
| Python 3.10+ | |
| PlatformIO (for building firmware) | `pip install platformio` |
| `pyserial` | `pip install -r simulator/qemu/requirements.txt` |

macOS (Homebrew sketch):

```bash
brew install ninja pkg-config glib pixman libgcrypt libslirp sdl2
python3 -m pip install -r simulator/qemu/requirements.txt
python3 -m pip install platformio
```

Linux (Debian/Ubuntu sketch):

```bash
sudo apt-get install -y build-essential ninja-build pkg-config \
  libglib2.0-dev libpixman-1-dev libgcrypt20-dev libslirp-dev libsdl2-dev \
  python3-pip python3-venv dosfstools
python3 -m pip install -r simulator/qemu/requirements.txt platformio
```

## First-time setup

```bash
git clone https://github.com/einklover/m4-stack.git
cd m4-stack
bash scripts/bootstrap_deps.sh          # open-m4-sdk etc.
./m4sim build-qemu -j $(nproc 2>/dev/null || sysctl -n hw.ncpu)
./m4sim info                            # must show murphy-m4 machine: yes
```

QEMU binary lands under:

```text
~/.cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa
```

Optional: `export QEMU_XTENSA=...` if you relocate it.

## Run a BIN

### A. Plugin-debug firmware (m4adb interactive — recommended)

```bash
./m4sim run --plugin-debug --build-qemu
# or leave session up:
./m4sim run --plugin-debug --keep-alive
```

### B. Existing app `firmware.bin`

```bash
# Needs matching bootloader/partitions from a PIO env:
pio run -e murphy_m4_qemu_plugin   # once
./m4sim run firmware/.pio/build/murphy_m4_qemu_plugin/firmware.bin \
  --build-dir firmware/.pio/build/murphy_m4_qemu_plugin
```

### B2. QEMU screen-only Home profile

`murphy_m4_qemu` is a different profile from `murphy_m4_qemu_plugin`: it uses
Quad PSRAM, renders the deterministic Home fixture, and emits the framebuffer
over UART before returning from setup. It does not initialize the m4adb bridge,
so do not run it through `m4sim run` (that path waits for a bridge and forces
the octal-PSRAM QEMU setting). Use the serial framebuffer runner instead:

```bash
pio run -e murphy_m4_qemu -j1
python3 simulator/tools/murphy_flash_image.py \
  --build-dir firmware/.pio/build/murphy_m4_qemu \
  -o /tmp/murphy-m4-qemu.bin
python3 simulator/qemu/run_murphy_bin.py /tmp/murphy-m4-qemu.bin \
  --seconds 40 \
  --serial-file /tmp/murphy-m4-qemu.serial.log \
  --screen-file /tmp/murphy-m4-qemu-home.pbm \
  --probe
```

The runner defaults to no `ssi_psram.is_octal` override, which is required for
this QSPI build. The plugin-debug profile remains the interactive m4adb path.

### C. Full 16 MiB flash image

```bash
python3 simulator/tools/murphy_flash_image.py \
  --build-dir firmware/.pio/build/murphy_m4 -o /tmp/m4-16m.bin
./m4sim run /tmp/m4-16m.bin
```

## Session commands

```bash
./m4sim screenshot out.png
./m4sim key back
./m4sim key enter
./m4sim ui                 # interactive viewer
./m4sim ui --json          # m4adb status
./m4sim stop
```

## Smoke test

```bash
./m4sim test smoke --plugin-debug
```

## What is not uploaded

- Patched QEMU build trees / binaries  
- Temporary flash/SD under `/tmp/m4sim`  
- Copyrighted ESP ROMs (bundled inside Espressif QEMU source, not this repo)  
- Secrets, Wi-Fi passwords, personal absolute IPs  

## Readiness definition

`READY` means:

1. QEMU announced a PTY  
2. `m4adb ping` returned JSON containing **`protocol` and `firmware`**  

TCP open alone is **not** ready. Fixed `sleep 10` is not used as the gate.

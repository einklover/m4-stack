# ESP32-S3 QEMU layer

This directory is the CPU/SoC execution layer of M4Sim. The deterministic
simulator remains responsible for reproducible SSD1677/SD/TLS timing, faults
and schedule fuzzing; Espressif QEMU executes real ESP32-S3 machine code.

There are now two QEMU paths:

1. **Smoke project** — compile/run the shared `PageTurnCoordinator` and query
   ESP-IDF heap state inside an ESP32-S3 guest.
2. **Full-flash runner** — boot a complete 16 MiB Murphy flash image containing
   the real bootloader, partition table and a QEMU-compatible Murphy application.

The second path executes the Murphy application on a Mac while keeping the
production profile separate from emulator-only compatibility changes. It is
not yet a complete Murphy board model:
unsupported SSD1677, SD, touch, frontlight or other board peripherals can still
stop the real firmware during HAL initialization.

## Prerequisites

Install/export ESP-IDF and Espressif's ESP32-S3 QEMU. Current Espressif docs:

- https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/tools/qemu.html

On macOS, Espressif documents the required QEMU runtime libraries and the
`idf_tools.py install qemu-xtensa` installation path. Source the ESP-IDF
`export.sh` before running the commands below.

The launcher project is pinned to `esp32s3`, 16 MiB flash, DIO and 40 MHz in
`CMakeLists.txt` + `sdkconfig.defaults`.

Plugin-debug runs keep `/tmp/m4-plugin-debug/artifacts/murphy-sd.img` across
restarts. Use `--fresh-sd` only when a deliberate wipe is needed. A runtime
TrueType font can be installed and selected for QEMU with:

```bash
python3 qemu/run_plugin_debug.py --font /path/to/font.ttf --keep-alive
```

The patched v3 QEMU exposes the SSD1677 framebuffer at a guest MMIO aperture.
Plugin-debug firmware copies each completed 800x480 monochrome frame there and
QEMU atomically publishes `artifacts/ssd1677-frame.pbm`. This avoids UART frame
dumps and byte-by-byte SPI overhead while retaining the firmware's real XML,
font and layout rendering. Open the interactive portrait viewer with the PTY
printed by the launcher:

```bash
python3 tools/m4_screen_viewer.py \
  --pty /dev/ttys006 \
  --frame-file /tmp/m4-plugin-debug/artifacts/ssd1677-frame.pbm
```

Clicks inject taps. Mouse drags inject coordinate swipes, so scrolling and
page-turn gestures use the same firmware input path as touch hardware. If the
direct frame file is unavailable, the viewer falls back to m4adb screenshots.

## A. Smoke project

```bash
cd qemu
idf.py build
idf.py qemu monitor
```

For GDB:

```bash
idf.py qemu gdb
```

Or monitor + a waiting GDB server:

```bash
idf.py qemu --gdb monitor
# second terminal
idf.py gdb
```

## B. Boot a full Murphy M4 flash image

### 1. Build the real firmware

From the Murphy firmware repository:

```bash
pio run -e murphy_m4_qemu
```

The production `murphy_m4` profile uses octal PSRAM and does not boot reliably
on Espressif QEMU 9.2.2. The QEMU-only profile uses Quad PSRAM, routes Arduino
`Serial` to UART0, and skips the unmodeled ADC self-calibration wait. Never
flash this profile to a real Murphy M4.

The current Murphy layout is 16 MiB with:

```text
0x000000  bootloader
0x008000  partition table
0x009000  nvs
0x00e000  otadata
0x010000  app0  (0x6d0000 bytes)
0x6e0000  app1  (0x6d0000 bytes)
0xdb0000  spiffs
0xff0000  coredump
```

The development firmware is constrained for the app1 slot at `0x6e0000`.

### 2. Compose a bootable 16 MiB image

From this simulator repository:

```bash
python3 tools/murphy_flash_image.py \
  --build-dir ../wap-checkpoint/firmware/.pio/build/murphy_m4_qemu \
  -o /tmp/murphy-m4-qemu.bin
```

With no base flash dump, `--slot auto` intentionally writes the same
`firmware.bin` into **both app0 and app1**. A blank QEMU instance therefore does
not depend on pre-existing OTA metadata to choose the application. The binary
being executed is still the exact PlatformIO application image.

The composer writes a sidecar JSON manifest containing every segment, offset,
size and SHA-256 hash.

### 3. Prefer a real factory flash dump when available

A raw 16 MiB dump is a more faithful board baseline because it preserves the
real bootloader, NVS, OTA data, stock app0 and SPIFFS. In that case `auto`
overlays only app1:

```bash
python3 tools/murphy_flash_image.py \
  --base-flash /path/to/murphy-factory-16m.bin \
  --firmware ../m4-firmware/.pio/build/murphy_m4/firmware.bin \
  -o /tmp/murphy-m4-qemu.bin
```

This is the preferred path for reproducing the real device boot selection and
persistent state without modifying the original dump.

### 4. Execute the image

```bash
python3 qemu/run_murphy_bin.py /tmp/murphy-m4-qemu.bin \
  --seconds 15 --serial-file /tmp/murphy-qemu.log \
  --screen-file /tmp/murphy-qemu-screen.pbm --probe
```

Success includes `[M4-RC1] setup() start`, `[M4-QEMU] screen bridge ready`, and
a 480x800 portrait PBM rendered by the real firmware UI stack. The QEMU-only
display adapter keeps the first frame stable because SSD1677, SDMMC, and input
GPIO are not modeled by Espressif QEMU. `run_full_flash.py` remains available
as the ESP-IDF-driven launcher alternative.

The runner validates that the image is exactly 16 MiB, builds this ESP-IDF
launcher project, then executes:

```text
idf.py -B build-fullflash qemu --flash-file <image> monitor
```

GDB and the Espressif virtual framebuffer can be enabled independently:

```bash
python3 qemu/run_full_flash.py /tmp/murphy-m4-qemu.bin --gdb
python3 qemu/run_full_flash.py /tmp/murphy-m4-qemu.bin --graphics
python3 qemu/run_full_flash.py /tmp/murphy-m4-qemu.bin --gdb --graphics
```

`--graphics` is Espressif's QEMU virtual framebuffer. It is **not** an SSD1677
model and the Murphy QEMU profile does not draw into it. Use `--screen-file`
to capture the firmware framebuffer adapter instead.

For CI or command inspection without ESP-IDF installed:

```bash
python3 qemu/run_full_flash.py /tmp/murphy-m4-qemu.bin --dry-run
```

## What success means at this stage

The full-flash path deliberately separates milestones:

```text
Stage 0  full 16 MiB image accepted by QEMU
Stage 1  ESP32-S3 ROM / second-stage bootloader executes
Stage 2  real Murphy application image reaches startup/app entry
Stage 3  unsupported Murphy board HAL calls are stubbed or modeled
Stage 4  firmware framebuffer commits can be exported as a portrait PBM
Stage 5  interactive buttons/touch/SD/network are modeled
```

The generic `murphy_m4_qemu` profile covers Stage 4 at the framebuffer
boundary. The patched `murphy_m4_qemu_plugin` path additionally covers the
interactive Stage 5 loop needed for plugin development: direct SSD1677 frame
publication, persistent SD, FT6x36-compatible touch/injected swipes and
open_eth networking. Electrical timing and panel waveform fidelity remain the
responsibility of deterministic M4Sim and hardware tests.

## Why this stays separate from deterministic M4Sim

QEMU gives instruction, linker, boot, MMU, FreeRTOS, exception and real compiled
code fidelity. Deterministic M4Sim gives controllable failure injection,
repeatable scheduling, real modeled heap fragmentation, EPD frame provenance
and thousands of fast seeds. Both layers are needed; neither should replace the
other.

# Stage 10 — SSD1677 on the real production display path

Branch: `agent/m4-emulator-stage10-ssd1677`

Parent stage: `agent/m4-emulator-stage9-gpspi2`

Date: 2026-08-11

## Goal

Replace the bootstrap `M4_QEMU_BUILD` framebuffer shortcut with a board-level
e-paper controller path used by the unchanged production guest:

```text
ESP-IDF / Arduino SPI2
  -> ESP32-S3 GP-SPI2 MMIO
  -> QEMU SSI bus
  -> Murphy SSD1677
  -> GPIO BUSY
  -> controller RAM / refresh
```

## Patch 0008

`0008-murphy-ssd1677.py` adds `hw/ssi/murphy_ssd1677.c`, a digital SSD1677
model with the firmware-visible contract already exercised by deterministic
M4Sim:

- 800x480 monochrome controller RAM (48,000 bytes);
- active-low SSI CS;
- GPIO DC and RESET inputs;
- BUSY output;
- X/Y range and counter registers;
- BW/red RAM write commands;
- soft reset and deep sleep;
- update-control state;
- MASTER_ACT-triggered asynchronous refresh;
- configurable virtual BUSY time;
- atomic commit from BW controller RAM to physical displayed frame.

The Murphy machine wires:

- SPI2 SSI bus -> SSD1677;
- GPIO5 -> CS;
- GPIO6 -> DC;
- GPIO7 -> RESET;
- SSD1677 BUSY -> GPIO8.

This matches the executable board contract rather than using a guest-side
screen hook.

## Controller-frame export

The SSD1677 exposes QOM properties:

```text
frame-file=/path/to/screen.pbm
busy-ms=40
```

On completed refresh it writes a binary PBM (`P4`) from the **emulated physical
controller frame**. SSD RAM uses 1=white while PBM uses 1=black, so export
inverts each byte.

`run_production_bin.py` now exposes these as:

```bash
--screen-file /tmp/murphy.pbm
--epd-busy-ms 40
```

The output is intentionally created only after a real MASTER_ACT + BUSY
completion. If production firmware never reaches/finishes a display refresh,
the runner reports that no screen was produced instead of manufacturing one.

## Scope boundary

This is a digital controller model, not electrophoretic material physics. It
does **not** attempt to simulate:

- analog gate/source waveforms;
- temperature-dependent particle mobility;
- visible ghosting accumulation;
- physical LUT voltage curves.

Those can remain a visualization/fault layer on top of controller behavior.
For binary compatibility, SPI command/data, RAM, BUSY, refresh atomicity and
sleep/reset behavior are the critical boundary.

## Known next risk: GP-SPI2 GDMA

Stage 9 initially implements CPU W0-buffer transactions. A full 48,000-byte
frame is commonly transferred in chunks and may use ESP32-S3 GDMA depending on
the Arduino/FreeInk SPI path. Therefore the first runtime test must distinguish:

1. controller command bytes reach SSD1677;
2. short register/data writes work;
3. large framebuffer transfer reaches the SSI bus completely;
4. MASTER_ACT starts BUSY and exports the expected frame.

If 1/2 pass while 3 stops, the missing layer belongs in the generic ESP32-S3
GP-SPI2/GDMA path, not in the SSD1677 slave.

## Acceptance

Compile/application gate:

- ordered QEMU patch series through 0008 applies cleanly;
- patched `xtensa-softmmu` builds;
- production-runner and patchset tests pass;
- `murphy-ssd1677` is registered and attachable on `-machine murphy-m4`.

Runtime production-BIN gate (requires private factory/production image):

```bash
python3 simulator/qemu/run_production_bin.py \
  /path/to/murphy-factory-16m.bin \
  --sd-image /path/to/card.img \
  --screen-file /tmp/murphy-screen.pbm \
  --serial-file /tmp/murphy.serial.log \
  --probe
```

Success means the unchanged guest writes its display through SPI2/SSD1677 and a
controller-generated PBM appears after BUSY completion. The legacy UART
`[M4-QEMU-FB]` screen bridge is not part of this acceptance path.

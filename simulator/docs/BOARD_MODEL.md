# Murphy M4 board-model fidelity

This layer sits below the deterministic reader model and above ESP32-S3 QEMU.
It is grounded in the current FreeInk `BoardConfig::MURPHY_M4` profile and the
project schematic rather than a generic ESP32-S3 dev-kit pinout.

## Source priority

1. **Firmware board profile + production-unit probe** — executable pin/controller truth.
2. **Schematic** — physical topology and devices that firmware may not enable yet.
3. **Field traces/issues** — timings, memory pressure, failure thresholds.
4. **Inference** — permitted only when explicitly tagged; never silently promoted.

`board/murphy_m4.json` carries the machine-readable contract and provenance.
`tools/validate_board_spec.py` rejects accidental GPIO conflicts and erased
source discrepancies.

## Current modeled layers

- GPIO ownership, pulls, external drive and contention.
- Active-low SD and touch power gates.
- EPD SPI bus transaction ownership and clock limits.
- SSD1677 command/data stream, BW/RED RAM planes, windows, update-control
  registers, custom-LUT upload, MASTER_ACTIVATION, active-high BUSY and atomic
  commit provenance.
- Shared I2C topology. The touch controller is firmware-visible at 0x2e; the
  schematic AHT20 is physically modeled at 0x38 but hidden from the firmware
  capability view because the current BoardConfig uses `NO_SENSORS`.
- Native four-bit SDMMC with mount/power/card-removal and in-flight power-loss
  failure injection.

## Fidelity boundaries

The SSD1677 model is **digital/controller-level**, not an electrophoretic-fluid
simulator. It can prove that firmware loaded the wrong RAM plane, used a stale
RED baseline, activated while BUSY, wrote an invalid window, selected the wrong
update sequence, or violated the single-commit provenance invariant. It cannot
predict analog ghost density, actual intermediate gray level, temperature
chemistry, or whether a particular LUT looks pleasing. Those remain calibrated
real-device tests.

Similarly, SDMMC models bus availability and asynchronous failure semantics;
FAT/exFAT directory allocation and rename durability are modeled separately at
the storage-transaction layer rather than pretending the bus itself implements
a filesystem.

## Known source discrepancy: flash capacity

The supplied schematic marks `U4` as `W25Q32JVSSIQ` (nominal 32 Mbit / 4 MiB),
while the current firmware target and factory partition map are 16 MiB (`N16R8`,
APP0 + APP1). The simulator therefore keeps a 16 MiB executable flash model but
records the schematic value as an unresolved discrepancy. A production-unit
JEDEC flash-ID/read-size trace should eventually close it; until then no code or
document should silently rewrite either source.

## Layer rule

Do not merge the board model into `SimPanel`. They answer different questions:

- `SimPanel`: fast deterministic reader/frame scheduling and provenance.
- `SimSsd1677Controller`: driver/register/RAM/BUSY correctness.
- ESP32-S3 QEMU: actual Xtensa/FreeRTOS/heap/flash execution.
- Real Murphy M4: RF, analog power, SD signal integrity, EPD physics and final
  calibration.

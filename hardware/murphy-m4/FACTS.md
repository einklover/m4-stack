# Murphy M4 verified hardware facts

Last updated: 2026-08-11

Primary schematic currently supplied by the device owner:
`SCH_ESP..18(2).pdf`, schematic title `ESP32_S3_6`, board field
`ESP32_426_S3_V2.0`, revision `V1.0`, created 2026-03-17 and updated
2026-05-06.

This file intentionally separates electrical evidence from assumptions used by
the firmware. A schematic value that conflicts with a shipping device must not
silently override measured/firmware evidence.

## Core SoC and memory

| Item | Fact | Evidence | Emulator consequence |
|---|---|---|---|
| MCU | U9 is labelled **ESP32-S3R8** | SCH | Xtensa ESP32-S3 machine; 8 MiB integrated PSRAM class is expected. |
| PSRAM | Shipping firmware profile is N16R8 and initializes 8 MiB Octal/OPI PSRAM | FW/SDK | Production-bin path must support S3 Octal PSRAM; Quad-QSPI is only a QEMU bootstrap profile. |
| External flash | Schematic U4 is labelled **W25Q32JVSSIQ** (32 Mbit / 4 MiB) | SCH | **Conflict:** current production layout and board profile use 16 MiB flash. Do not use the U4 capacity as the emulator contract until the shipping PCB/flash ID is measured. |
| Crystal | X1 is 40 MHz | SCH | QEMU clock/boot assumptions should remain ESP32-S3 40 MHz compatible. |
| Boot straps | Schematic notes GPIO0 pull-up, GPIO45 pull-down, GPIO46 pull-down; GPIO3 floating | SCH | Preserve SPI-flash boot strap behavior; current QEMU runner uses strap mode `0x04`. |

## E-paper interface

The schematic exposes a direct display/panel interface with named nets:

- `CLK`
- `DIN`
- `CS`
- `DC`
- `RST`
- `BUSY`
- `PREVGH`
- `PREVGL`
- `GDR`
- `RESE`

Evidence: **SCH**.

The firmware/FreeInk stack identifies the display controller path as SSD1677.
That controller identity is therefore **SDK/FW evidence**, not inferred solely
from the PDF. The binary emulator must eventually model SPI command/data
selection, reset, BUSY timing and panel RAM/refresh semantics; exporting a
framebuffer from a QEMU-only firmware shim is not equivalent hardware.

## microSD / SDMMC

The second schematic page is dedicated to the TF socket. The following 4-bit
SDMMC signals are present (**SCH**):

- `SD_CLK`
- `SD_CMD`
- `SD_DATA0`
- `SD_DATA1`
- `SD_DATA2`
- `SD_DATA3`
- `SD_CD`
- `SD_PWR`
- `SD_PWR_IN`

A Q5-controlled power path and pull resistors are shown around the socket.
This confirms that the emulator should not model the card as an always-present
abstract filesystem only: card power and card-detect state are observable board
inputs. The first useful QEMU implementation may still default to
`powered + inserted`, but those states must be injectable.

## Touch

FPC3 is labelled `触屏` (touch screen). The board-side interface exposes
(**SCH**):

- `SCL`
- `SDA`
- `TOUCH_INT`
- `TOUCH_PWR`
- `I2C_DEV_VDD`

Q6 and surrounding passives form a controllable touch-power path. The touch
controller itself is on/behind the FPC and is not identified by this schematic,
so its register protocol must be verified from the SDK, bus capture or panel
part number before a register-accurate QEMU I2C device is written.

## Frontlight

The schematic contains separate cold/warm LED channels (**SCH**):

- `LED_C+`, `LED_C-`
- `LED_W+`, `LED_W-`
- `BL_C_PWM`
- `BL_W_PWM`

U1 and U16 are labelled **AW9967DNR** boost/LED driver devices with 10 uH
inductors. FPC2 is labelled frontlight connector. The simulator should expose
both independent PWM channels and preserve on/off/brightness/warmth state even
when it does not simulate analog boost-current physics.

## Sensors and I2C devices

| Device / block | Schematic evidence | Notes |
|---|---|---|
| SC7A20HTR | U5, labelled attitude sensor | Connected to `SDA`, `SCL`, `SC_VDD`; interrupt pins are present on the IC. |
| AHT20 | U8 | I2C temperature/humidity sensor on `SCL`/`SDA`, 3.3 V. |
| U6 clock/IRQ block | 8-pin device with SDA/SCL and `/IRQ1`, `/IRQ2` | Exact part number is not legible in the supplied schematic; keep **unresolved**. |

All rows above are **SCH** evidence. Do not invent I2C addresses; resolve them
from BOM/SDK or a real-device bus trace.

## Power, battery and charging

- `VBAT`, `VBUS`, `BAT_ADC` are explicit nets. **SCH**
- U3 is labelled `ME6210A33M3G`, a 3.3 V LDO. **SCH**
- U7 is labelled `TP4056`; charging/status nets include `CHRG`. **SCH**
- Battery/USB source switching, a battery switch and a reset button are shown.
  **SCH**

For early emulation, battery voltage and USB-present can be synthetic values.
For production-bin fidelity they should be exposed as host-settable state and
feed the same ADC/GPIO path used by firmware.

## Buttons, buzzer and other named nets

The schematic names `KEY1`, `KEY2`, `KEY_LOCK`, `BUZ_PWM` and shows four
TS3819ZJ push-button symbols plus a buzzer block marked 4 kHz. **SCH**

Audio-related named nets also appear (`AUDIO_BCLK`, `AUDIO_MCLK`,
`AUDIO_RCLK`, `AUDIO_I2S_DI`, `AUDIO_I2S_DO`). Their populated peripheral and
shipping-device role are not yet verified; treat them as **SCH-only** until
cross-checked.

## Known conflicts / unresolved facts

1. **Flash capacity conflict:** schematic says W25Q32 (4 MiB), while the current
   Murphy production partition map and N16R8 profile use 16 MiB. Resolve by
   reading JEDEC ID / full flash size from a shipping unit and, ideally, adding
   a PCB photo/BOM revision.
2. **Touch controller identity:** board nets are known; controller model/address
   is not yet verified from this PDF.
3. **U6 identity:** clock/IRQ I2C block needs BOM/SDK confirmation.
4. **Exact GPIO-number map:** named nets are visible, but GPIO assignments should
   be generated from the FreeInk `BoardConfig` and cross-checked against the
   schematic before being frozen as a QEMU machine definition.
5. **Display controller identity:** SSD1677 is established by the firmware/SDK
   path, while the supplied schematic mainly exposes panel/driver nets. Keep
   both evidence sources linked.

## Hardware capture requests that remove the remaining ambiguity

When convenient, preserve these artifacts under this directory:

- shipping-board flash JEDEC ID + 16 MiB raw flash dump;
- eFuse dump (with secret/key material redacted when applicable);
- FreeInk/Murphy BoardConfig pin table;
- touch controller ID/register probe or I2C trace;
- boot-time logic-analyzer captures for SDMMC, display SPI and touch I2C;
- PCB/BOM revision identifying the exact production flash and touch parts.

# Murphy M4 electrical nets

This is the human-readable companion to `simulator/board/MurphyM4Spec.h`.
Named nets come from the 2026-05-06 `ESP32_426_S3_V2.0` schematic unless a
row explicitly says otherwise. GPIO numbers come from the executable simulator
contract, which was derived from the Murphy BoardConfig/live-probed firmware.

## Display / e-paper

| Net | GPIO | Direction from ESP32 | Notes |
|---|---:|---|---|
| CLK | 4 | out | display SPI SCLK |
| DIN | 3 | out | display SPI MOSI |
| CS | 5 | out | active-low chip select |
| DC | 6 | out | command/data select |
| RST | 7 | out | panel/controller reset |
| BUSY | 8 | in | current executable contract treats BUSY as active-high |
| PREVGH | — | analog/power | panel high-voltage rail control path; not a CPU GPIO contract yet |
| PREVGL | — | analog/power | panel low-voltage rail control path; not a CPU GPIO contract yet |
| GDR | — | analog/power | panel gate-drive path |
| RESE | — | analog/power | panel source/gate power path |

Controller identity (`SSD1677`) is SDK/FW evidence rather than a readable part
number on the supplied schematic.

## SD / TF socket

| Net | GPIO | Direction | Notes |
|---|---:|---|---|
| SD_PWR | 10 | out | active-low switched card power in current BoardConfig contract |
| SD_CLK | 16 | out | SDMMC clock |
| SD_CMD | 15 | bidirectional | SD command |
| SD_DATA0 | 17 | bidirectional | data bit 0 |
| SD_DATA1 | 18 | bidirectional | data bit 1 |
| SD_DATA2 | 11 | bidirectional | data bit 2 |
| SD_DATA3 | 14 | bidirectional | data bit 3 |
| SD_CD | **unresolved** | in | card-detect net is explicit on schematic; exact GPIO still needs BoardConfig/measurement confirmation |
| SD_PWR_IN | — | rail | switched card supply after the power gate |

The physical topology is therefore 4-bit SDMMC with observable card power and
card presence, not a permanently mounted abstract filesystem.

## Touch / shared I2C

| Net | GPIO | Direction | Notes |
|---|---:|---|---|
| SDA | 13 | bidirectional open-drain | shared I2C |
| SCL | 12 | bidirectional/open-drain | shared I2C clock |
| TOUCH_INT | 44 | in | touch IRQ |
| TOUCH_PWR | 45 | out | active-low touch rail in executable contract |
| I2C_DEV_VDD | — | rail | touch/device supply |

Current firmware-compatible simulator behavior is FT6x36/FT6336-like at I2C
address `0x2e`; exact panel touch-controller part number remains to be verified.

Other schematic I2C devices include AHT20 (`0x38` in the executable contract),
SC7A20 and an unresolved U6 clock/IRQ device. The shipping firmware currently
does not expose every physically drawn sensor.

## Buttons / wake

| Logical function | GPIO | Active level |
|---|---:|---|
| KEY1 / Up | 1 | low |
| KEY2 / Down | 2 | low |
| KEY_LOCK / Power | 0 | low |

GPIO0 is also a boot strap, so emulator reset/wake behavior must distinguish
power-key level during normal execution from reset-time strap sampling.

## Battery / power

| Net | GPIO | Notes |
|---|---:|---|
| BAT_ADC | 9 | battery ADC; executable contract uses 2.0 divider multiplier |
| VBUS | — | USB supply state |
| VBAT | — | cell supply |
| CHRG | 43 on schematic/old capability | current firmware intentionally disables this status input |

The schematic also shows TP4056 charging, a 3.3 V LDO and battery/USB source
switching. Initial emulator fidelity can treat these as digital/ADC state; no
analog charger simulation is required for production firmware execution.

## Frontlight and buzzer

| Net | GPIO | Notes |
|---|---:|---|
| BL_W_PWM | 47 | warm-channel PWM in current executable contract |
| BL_C_PWM | 48 | cool-channel PWM in current executable contract |
| BUZ_PWM | 46 | buzzer PWM |

The schematic shows separate AW9967DNR LED boost channels for warm/cool LEDs.
The deterministic model currently uses 5 kHz, 8-bit PWM semantics.

## Evidence discipline

A GPIO row is not frozen merely because a named net exists in the schematic.
The GPIO numbers above are sourced from the existing executable board contract;
new/revised board drawings must be cross-checked against BoardConfig or a live
logic probe before changing them. `SD_CD` is deliberately left unresolved
rather than guessed.

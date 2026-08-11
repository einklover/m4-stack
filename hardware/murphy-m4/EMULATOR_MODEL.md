# Murphy M4 emulator model matrix

The project has two complementary execution layers:

- **Deterministic M4Sim** — fast, repeatable peripheral/protocol/fault model.
- **Espressif QEMU** — executes the real ESP32-S3 ROM, bootloader, FreeRTOS and
  compiled application machine code.

The binary-level target is reached when an unmodified production flash image
boots while QEMU/board devices provide the hardware behavior. Deterministic
M4Sim remains the executable reference for protocol semantics and fault tests.

| Hardware | Deterministic M4Sim | QEMU production-bin path | Next contract |
|---|---|---|---|
| Xtensa/ROM/MMU | not CPU-emulated | Espressif ESP32-S3 QEMU | retain upstream fidelity |
| 16 MiB flash layout | fixture-level | full MTD image runner | verify shipping JEDEC ID |
| 8 MiB Octal PSRAM | capacity/policy | upstream QEMU path currently incomplete for Murphy production image | fix MSPI/OPI path below guest |
| ADC startup calibration | policy only | upstream QEMU lacks required completion behavior in current tested version | QEMU ADC calibration completion model |
| SD power | active-low gate modeled | GPIO only | wire gate to SD card device availability |
| SD card detect | logical `inserted` state | unmodeled | resolve GPIO then drive it from host insert/remove |
| SDMMC 4-bit | mount/timing/power-loss/faults + raw sector image backing | unmodeled at Murphy boundary | QEMU ESP32-S3 SDMMC + raw `sdcard.img` |
| SSD1677 | register/RAM/update/BUSY/atomic commit modeled | guest framebuffer shim only | QEMU SPI slave + DC/RST/BUSY GPIO adapter |
| Touch | FT6x36-compatible register/frame model, IRQ/power policy | unmodeled | QEMU I2C device + IRQ/power gate |
| Keys | active-low debounce/wake modeled | floating/unmodeled | host key injection -> GPIO levels/interrupts |
| Battery | cell voltage/divider/percentage policy | unmodeled | host voltage -> ADC conversion |
| Frontlight | 2-channel PWM split modeled | unmodeled | observe LEDC/PWM and expose host state |
| Deep sleep/wake | rail-hold and power-button wake modeled | partial SoC support only | preserve RTC/GPIO wake semantics |
| AHT20 | physical I2C placeholder at 0x38 | unmodeled | optional, firmware-disabled today |
| SC7A20 | presence/capability only | unmodeled | optional until firmware enables it |
| Wi-Fi | network behavior can be fault-driven above transport | `open_eth` is not ESP Wi-Fi | functional ESP Wi-Fi compatibility backend |
| eFuse | not CPU-visible | QEMU backing file supported | preserve per-device state safely |

## Shared-contract rule

A QEMU device should not invent a second set of Murphy timings, dimensions,
polarity or pin numbers. Where practical, values are exported from
`simulator/board/MurphyM4Spec.h` by the `m4_board_contract` executable. QEMU
patch/build scripts should consume a generated contract or copy constants with a
compile/test assertion that makes divergence visible.

## SD image contract

`SimSdCardImage` treats a card as a raw 512-byte-sector image. Filesystem and
partition semantics belong to the guest. This matches the desired QEMU CLI
shape:

```text
--sd-image /path/to/sdcard.img
```

The device model must separately preserve:

1. image bytes/capacity;
2. card insertion (`SD_CD` once its GPIO is resolved);
3. switched power (`SD_PWR`);
4. bus-width negotiation (4-bit for Murphy);
5. operation completion/interrupt/DMA state;
6. power-loss/removal invalidation.

The deterministic model already covers items 1, 3, 4 and 6 at protocol-policy
level; QEMU must provide the ESP32-S3 controller/MMIO/DMA side for production
firmware.

## Display contract

`SimSsd1677Controller` is intentionally a digital controller model, not a
physical electrophoresis simulator. For binary compatibility the QEMU-facing
SSD1677 device needs only to be accurate at the firmware-visible boundary:
SPI command/data, controller RAM/windowing, RST, BUSY and update completion.
Ghosting/LUT/temperature physics can remain an optional visualization layer.

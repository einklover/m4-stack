#!/usr/bin/env python3
"""Apply Murphy patch 0001 to a pinned Espressif QEMU source tree.

This is a context-checked source transformation rather than a hand-authored
unified diff. It aborts if the pinned upstream source no longer matches every
expected fragment, which makes rebases explicit while avoiding fragile manual
hunk line counts.
"""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one upstream match, found {count}")
    return text.replace(old, new, 1)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} QEMU_SOURCE", file=sys.stderr)
        return 2
    root = Path(argv[1]).resolve()
    path = root / "hw/xtensa/esp32s3.c"
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#define ESP32S3_IO_WARNING  0\n\ntypedef struct Esp32s3SocState {",
        """#define ESP32S3_IO_WARNING  0

/* Minimal RTC/SENS ADC register model used by ESP-IDF oneshot calibration. */
#define ESP32S3_SENS_REG_SIZE          0x100
#define ESP32S3_SENS_ADC1_CTRL2_OFF    0x0c
#define ESP32S3_SENS_ADC2_CTRL2_OFF    0x30
#define ESP32S3_SENS_ADC_START         BIT(17)
#define ESP32S3_SENS_ADC_DONE          BIT(16)
#define ESP32S3_SENS_ADC_DATA_MASK     0x0000ffffu

typedef struct Esp32s3SocState {""",
        "insert SENS constants",
    )

    text = replace_once(
        text,
        "    DeviceState *eth;\n    SsiPsramState *psram;\n\n    uint32_t requested_reset;",
        """    DeviceState *eth;
    SsiPsramState *psram;

    uint32_t sens_regs[ESP32S3_SENS_REG_SIZE / sizeof(uint32_t)];

    uint32_t requested_reset;""",
        "insert SENS state",
    )

    text = replace_once(
        text,
        """        for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
            device_cold_reset(DEVICE(&s->uart[i]));
        }
    }
    if (s->requested_reset & ESP32S3_SOC_RESET_PROCPU) {""",
        """        for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
            device_cold_reset(DEVICE(&s->uart[i]));
        }
        memset(s->sens_regs, 0, sizeof(s->sens_regs));
    }
    if (s->requested_reset & ESP32S3_SOC_RESET_PROCPU) {""",
        "reset SENS state",
    )

    old_io = """static uint64_t esp32s3_io_read(void *opaque, hwaddr addr, unsigned int size)
{
#if ESP32S3_IO_WARNING
    warn_report("[ESP32-S3] Unsupported read to $%08lx, size = %i\\n", ESP32S3_IO_START_ADDR + addr, size);
#endif
    return 0;
}


static void esp32s3_io_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
#if ESP32S3_IO_WARNING
        warn_report("[ESP32-S3] Unsupported write $%08lx = %08lx\\n", ESP32S3_IO_START_ADDR + addr, value);
#endif
}
"""
    new_io = """static uint64_t esp32s3_io_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3SocState *s = ESP32S3_SOC(opaque);
    const hwaddr phys = ESP32S3_IO_START_ADDR + addr;

    if (size == sizeof(uint32_t) &&
        phys >= DR_REG_SENS_BASE &&
        phys < DR_REG_SENS_BASE + ESP32S3_SENS_REG_SIZE) {
        return s->sens_regs[(phys - DR_REG_SENS_BASE) / sizeof(uint32_t)];
    }
#if ESP32S3_IO_WARNING
    warn_report("[ESP32-S3] Unsupported read to $%08lx, size = %i\\n", ESP32S3_IO_START_ADDR + addr, size);
#endif
    return 0;
}


static void esp32s3_io_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    Esp32s3SocState *s = ESP32S3_SOC(opaque);
    const hwaddr phys = ESP32S3_IO_START_ADDR + addr;

    if (size == sizeof(uint32_t) &&
        phys >= DR_REG_SENS_BASE &&
        phys < DR_REG_SENS_BASE + ESP32S3_SENS_REG_SIZE) {
        const unsigned index = (phys - DR_REG_SENS_BASE) / sizeof(uint32_t);
        const hwaddr off = phys - DR_REG_SENS_BASE;
        const uint32_t old = s->sens_regs[index];
        uint32_t next = (uint32_t)value;

        if (off == ESP32S3_SENS_ADC1_CTRL2_OFF ||
            off == ESP32S3_SENS_ADC2_CTRL2_OFF) {
            next &= ~(ESP32S3_SENS_ADC_DONE | ESP32S3_SENS_ADC_DATA_MASK);
            next |= old & (ESP32S3_SENS_ADC_DONE | ESP32S3_SENS_ADC_DATA_MASK);
            if (!(value & ESP32S3_SENS_ADC_START)) {
                next &= ~ESP32S3_SENS_ADC_DONE;
            } else if (!(old & ESP32S3_SENS_ADC_START)) {
                next &= ~ESP32S3_SENS_ADC_DATA_MASK;
                next |= ESP32S3_SENS_ADC_DONE;
            }
        }
        s->sens_regs[index] = next;
        return;
    }
#if ESP32S3_IO_WARNING
    warn_report("[ESP32-S3] Unsupported write $%08lx = %08lx\\n", ESP32S3_IO_START_ADDR + addr, value);
#endif
}
"""
    text = replace_once(text, old_io, new_io, "replace generic I/O handlers")

    text = replace_once(
        text,
        """    memory_region_init_io(&ss->iomem, OBJECT(&ss->cpu[0]), &esp32s3_io_ops,
                          NULL, "esp32s3.iomem", 0xd1000);""",
        """    memory_region_init_io(&ss->iomem, OBJECT(&ss->cpu[0]), &esp32s3_io_ops,
                          ss, "esp32s3.iomem", 0xd1000);""",
        "pass SoC as I/O opaque",
    )

    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

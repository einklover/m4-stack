#!/usr/bin/env python3
"""Instantiate Espressif QEMU's existing ESP32 I2C core on ESP32-S3."""
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
        '#include "hw/i2c/i2c.h"\n#include "hw/qdev-properties.h"',
        '#include "hw/i2c/i2c.h"\n#include "hw/i2c/esp32_i2c.h"\n#include "hw/qdev-properties.h"',
        "include ESP32 I2C controller",
    )

    text = replace_once(
        text,
        "    ESP32S3UARTState uart[ESP32S3_UART_COUNT];\n    ESP32S3GPIOState gpio;",
        "    ESP32S3UARTState uart[ESP32S3_UART_COUNT];\n    Esp32I2CState i2c[2];\n    ESP32S3GPIOState gpio;",
        "add two S3 I2C controller states",
    )

    text = replace_once(
        text,
        """        for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
            device_cold_reset(DEVICE(&s->uart[i]));
        }
""",
        """        for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
            device_cold_reset(DEVICE(&s->uart[i]));
        }
        for (int i = 0; i < 2; ++i) {
            device_cold_reset(DEVICE(&s->i2c[i]));
        }
""",
        "reset S3 I2C controllers",
    )

    text = replace_once(
        text,
        """    for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
        snprintf(name, sizeof(name), "uart%d", i);
        object_initialize_child(obj, name, &s->uart[i], TYPE_ESP32S3_UART);
    }

    object_property_add_alias(obj, "serial0", OBJECT(&s->uart[0]), "chardev");""",
        """    for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
        snprintf(name, sizeof(name), "uart%d", i);
        object_initialize_child(obj, name, &s->uart[i], TYPE_ESP32S3_UART);
    }
    for (int i = 0; i < 2; ++i) {
        snprintf(name, sizeof(name), "i2c%d", i);
        object_initialize_child(obj, name, &s->i2c[i], TYPE_ESP32_I2C);
    }

    object_property_add_alias(obj, "serial0", OBJECT(&s->uart[0]), "chardev");""",
        "initialize S3 I2C children",
    )

    anchor = """    /* GPIO realization */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ss->gpio), &error_fatal);"""
    block = """    /* I2C controller realization. The generic ESP32 controller model uses
     * the register layout shared by ESP32-S3 for the master/FIFO/command path.
     */
    for (int i = 0; i < 2; ++i) {
        const hwaddr base = i == 0 ? DR_REG_I2C_EXT_BASE : DR_REG_I2C1_EXT_BASE;
        const int irq_source = i == 0 ? ETS_I2C_EXT0_INTR_SOURCE : ETS_I2C_EXT1_INTR_SOURCE;
        sysbus_realize(SYS_BUS_DEVICE(&ss->i2c[i]), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->i2c[i]), 0);
        memory_region_add_subregion_overlap(sys_mem, base, mr, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(&ss->i2c[i]), 0,
                           qdev_get_gpio_in(intmatrix_dev, irq_source));
    }

    /* GPIO realization */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ss->gpio), &error_fatal);"""
    text = replace_once(text, anchor, block, "realize S3 I2C controllers")

    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Add ESP32-S3 GP-SPI2 against the cumulative Murphy patch state.

The original 0007 was authored against an older esp32s3.c layout.  In the
pinned QEMU tree the peripheral children are initialized inside machine_init
with OBJECT(ss), and extmem precedes spi1.  Keep the generated controller source
from 0007, but apply integration edits against the actual cumulative v3 state.
"""
from __future__ import annotations

from pathlib import Path
import runpy
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one cumulative match, found {count}")
    return text.replace(old, new, 1)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        return 2
    root = Path(argv[1]).resolve()
    here = Path(__file__).resolve().parent
    legacy = runpy.run_path(str(here / "0007-esp32s3-gpspi2.py"))
    gpspi_source = legacy["GPSPI_SOURCE"]

    source = root / "hw/ssi/esp32s3_gpspi.c"
    if source.exists():
        raise RuntimeError(f"refusing to overwrite upstream file: {source}")
    source.write_text(gpspi_source, encoding="utf-8")

    meson = root / "hw/ssi/meson.build"
    mtext = meson.read_text(encoding="utf-8")
    mtext = replace_once(
        mtext,
        "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c'))",
        "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c', 'esp32s3_gpspi.c'))",
        "compile S3 GP-SPI2",
    )
    meson.write_text(mtext, encoding="utf-8")

    path = root / "hw/xtensa/esp32s3.c"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '#define TYPE_MURPHY_FT6X36 "murphy-ft6x36"\n',
        '#define TYPE_MURPHY_FT6X36 "murphy-ft6x36"\n#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"\n',
        "define GP-SPI2 type",
    )
    text = replace_once(
        text,
        "    SsiPsramState *psram;\n\n    uint32_t sens_regs",
        "    SsiPsramState *psram;\n    DeviceState *gpspi2;\n\n    uint32_t sens_regs",
        "add GP-SPI2 state",
    )

    # The pinned S3 machine initializes children in machine_init after the SoC
    # itself is realized. Attach the dynamic GP-SPI2 child immediately after the
    # existing SPI1 child, using the real OBJECT(ss) parent.
    spi1 = '    object_initialize_child(OBJECT(ss), "spi1", &ss->spi1, TYPE_ESP32S3_SPI);\n'
    text = replace_once(
        text,
        spi1,
        spi1
        + '    ss->gpspi2 = qdev_new(TYPE_ESP32S3_GPSPI);\n'
        + '    object_property_add_child(OBJECT(ss), "gpspi2", OBJECT(ss->gpspi2));\n',
        "create GP-SPI2 child",
    )

    anchor = "    /* I2C controller realization. The generic ESP32 controller model uses\n"
    block = """    /* General-purpose SPI2 used by the Murphy display stack. */
    sysbus_realize(SYS_BUS_DEVICE(ss->gpspi2), &error_fatal);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI2_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(ss->gpspi2), 0), 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(ss->gpspi2), 0,
        qdev_get_gpio_in(intmatrix_dev, ETS_SPI2_INTR_SOURCE));

""" + anchor
    text = replace_once(text, anchor, block, "realize GP-SPI2")
    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

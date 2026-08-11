#!/usr/bin/env python3
"""Map ESP32-S3 SPI0 (MSPI cache-side) onto the existing SPI1 controller.

Upstream Espressif QEMU only realizes SPI1 at 0x60002000. Production second-
stage bootloaders and MSPI timing paths also poke SPI0 at 0x60003000. Those
accesses currently fall into the catch-all iomem window (always return 0 /
ignore writes), which leaves the guest spinning after entry while APP CPU waits
in ROM for ets_set_user_start.

Alias SPI0's MMIO onto SPI1 so both register windows share the same digital
SPI/flash/PSRAM model. This is the same bus on silicon; separate CS/timing
state can be refined later if a real dual-controller race appears.
"""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        return 2
    root = Path(argv[1]).resolve()
    path = root / "hw/xtensa/esp32s3.c"
    text = path.read_text(encoding="utf-8")

    anchor = """        ss->spi1.xts_aes = &ss->xts_aes;
        sysbus_realize(SYS_BUS_DEVICE(&ss->spi1), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->spi1), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI1_BASE, mr, 0);
"""
    replacement = """        ss->spi1.xts_aes = &ss->xts_aes;
        sysbus_realize(SYS_BUS_DEVICE(&ss->spi1), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->spi1), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI1_BASE, mr, 0);
        /* SPI0 (MSPI) shares the flash/PSRAM bus; expose the same controller
         * window so production bootloaders that program SPI0 do not hang. */
        {
            MemoryRegion *spi0 = g_new(MemoryRegion, 1);
            memory_region_init_alias(spi0, OBJECT(&ss->spi1), "esp32s3.spi0",
                                     mr, 0, memory_region_size(mr));
            memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI0_BASE, spi0, 0);
        }
"""
    text = replace_once(text, anchor, replacement, "alias SPI0 onto SPI1")
    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Make ESP32-S3 AES+GDMA match silicon so TLS works without firmware shims.

Three SoC-model bugs make mbedtls hardware AES hang in QEMU
(qemu.log: "[AES] Error reading from GDMA buffer"):

1. Channel lookup ORs peri_sel with LINK.START, so AES can pick a leftover
   SPI/LCD channel and read garbage descriptors.
2. GDMA's address space is only the DRAM MemoryRegion, so IRAM/PSRAM buffers
   used by DMA descriptors fail dma_memory_read.
3. IRAM (0x40370000) and DRAM (0x3FC80000) are separate RAM objects. On the
   real S3 they are the same SRAM. Hardware GDMA reconstructs only a 20-bit
   DRAM address; CPU stores to the I-bus window must be visible there.

This is a generic ESP32-S3 QEMU correctness fix, not a Murphy firmware shim.
"""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one upstream match, found {count}")
    return text.replace(old, new, 1)


def patch_gdma(root: Path) -> None:
    path = root / "hw/dma/esp_gdma.c"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        """        if ( FIELD_EX32(s->ch_conf[dir][i].peripheral, GDMA_PERI_SEL, PERI_SEL) == periph ||
             FIELD_EX32(s->ch_conf[dir][i].link, GDMA_OUT_LINK, START)) {

            *chan = i;
            return true;
        }""",
        """        /* peri_sel is the only correct bind. OR-ing START matched leftover
         * SPI/LCD channels and sent AES/SHA at the wrong descriptor list. */
        if (FIELD_EX32(s->ch_conf[dir][i].peripheral, GDMA_PERI_SEL, PERI_SEL) == periph) {
            *chan = i;
            return true;
        }""",
        "gdma peri_sel match",
    )
    path.write_text(text, encoding="utf-8")


def patch_soc(root: Path) -> None:
    path = root / "hw/xtensa/esp32s3.c"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        """    DeviceState *eth;
    SsiPsramState *psram;
    DeviceState *gpspi2;""",
        """    DeviceState *eth;
    SsiPsramState *psram;
    DeviceState *gpspi2;
    MemoryRegion *dram;""",
        "soc dram pointer",
    )
    text = replace_once(
        text,
        """    memory_region_init_ram(dram, NULL, "esp32s3.dram",
                           memmap[ESP32S3_MEMREGION_DRAM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_DRAM].base, dram);""",
        """    memory_region_init_ram(dram, NULL, "esp32s3.dram",
                           memmap[ESP32S3_MEMREGION_DRAM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_DRAM].base, dram);
    ss->dram = dram;""",
        "store dram on soc",
    )
    text = replace_once(
        text,
        """    memory_region_init_ram(iram, NULL, "esp32s3.iram",
                           memmap[ESP32S3_MEMREGION_IRAM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_IRAM].base, iram);""",
        """    /* I-bus SRAM is the same physical memory as D-bus DRAM. AES/GDMA
     * reconstructs only a 20-bit DRAM address; alias so I-bus stores are
     * visible to GDMA. */
    memory_region_init_alias(iram, NULL, "esp32s3.iram",
                             s->dram, 0, memmap[ESP32S3_MEMREGION_IRAM].size);
    memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_IRAM].base, iram);""",
        "alias iram onto dram",
    )
    text = replace_once(
        text,
        """        object_property_set_link(OBJECT(&ss->gdma), "soc_mr", OBJECT(dram), &error_abort);""",
        """        /* Same address map the CPU sees: DRAM, IRAM alias, PSRAM. */
        object_property_set_link(OBJECT(&ss->gdma), "soc_mr", OBJECT(sys_mem), &error_abort);""",
        "gdma uses system memory",
    )
    path.write_text(text, encoding="utf-8")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} QEMU_SOURCE", file=sys.stderr)
        return 2
    root = Path(argv[1]).resolve()
    patch_gdma(root)
    patch_soc(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

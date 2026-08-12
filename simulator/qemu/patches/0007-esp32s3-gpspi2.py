#!/usr/bin/env python3
"""Add an ESP32-S3 GP-SPI2 controller with the S3 register layout."""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one upstream match, found {count}")
    return text.replace(old, new, 1)


GPSPI_SOURCE = r'''#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"
#include "qemu/module.h"

#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3GpSpiState, ESP32S3_GPSPI)

#define GPSPI_MMIO_SIZE 0x1000
#define GPSPI_REG_WORDS (GPSPI_MMIO_SIZE / 4)
#define GPSPI_FIFO_WORDS 16
#define R_CMD 0x00
#define R_ADDR 0x04
#define R_USER 0x10
#define R_USER1 0x14
#define R_USER2 0x18
#define R_MS_DLEN 0x1c
#define R_DMA_INT_ENA 0x34
#define R_DMA_INT_CLR 0x38
#define R_DMA_INT_RAW 0x3c
#define R_DMA_INT_ST 0x40
#define R_DMA_INT_SET 0x44
#define R_W0 0x98
#define CMD_USR BIT(24)
#define CMD_UPDATE BIT(23)
#define USER_COMMAND BIT(31)
#define USER_ADDR BIT(30)
#define USER_DUMMY BIT(29)
#define USER_MISO BIT(28)
#define USER_MOSI BIT(27)
#define INT_TRANS_DONE BIT(12)

struct ESP32S3GpSpiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    SSIBus *spi;
    uint32_t regs[GPSPI_REG_WORDS];
};

static inline uint32_t *gpspi_reg(ESP32S3GpSpiState *s, hwaddr addr)
{
    return &s->regs[addr >> 2];
}

static void gpspi_update_irq(ESP32S3GpSpiState *s)
{
    qemu_set_irq(s->irq, (s->regs[R_DMA_INT_RAW >> 2] & s->regs[R_DMA_INT_ENA >> 2]) != 0);
}

static uint8_t gpspi_fifo_get(ESP32S3GpSpiState *s, unsigned index)
{
    uint8_t *buf = (uint8_t *)&s->regs[R_W0 >> 2];
    return index < GPSPI_FIFO_WORDS * 4 ? buf[index] : 0;
}

static void gpspi_fifo_set(ESP32S3GpSpiState *s, unsigned index, uint8_t value)
{
    uint8_t *buf = (uint8_t *)&s->regs[R_W0 >> 2];
    if (index < GPSPI_FIFO_WORDS * 4) {
        buf[index] = value;
    }
}

static void gpspi_send_msb(ESP32S3GpSpiState *s, uint32_t value, unsigned bits)
{
    unsigned bytes = (bits + 7) / 8;
    if (bytes > 4) {
        bytes = 4;
    }
    for (unsigned i = 0; i < bytes; ++i) {
        unsigned shift = (bytes - 1 - i) * 8;
        ssi_transfer(s->spi, (value >> shift) & 0xff);
    }
}

static void gpspi_transaction(ESP32S3GpSpiState *s)
{
    const uint32_t user = s->regs[R_USER >> 2];
    const uint32_t user1 = s->regs[R_USER1 >> 2];
    const uint32_t user2 = s->regs[R_USER2 >> 2];
    if (user & USER_COMMAND) {
        gpspi_send_msb(s, user2 & 0xffff, ((user2 >> 28) & 0xf) + 1);
    }
    if (user & USER_ADDR) {
        gpspi_send_msb(s, s->regs[R_ADDR >> 2], ((user1 >> 27) & 0x1f) + 1);
    }
    if (user & USER_DUMMY) {
        unsigned cycles = (user1 & 0xff) + 1;
        for (unsigned i = 0; i < (cycles + 7) / 8; ++i) {
            ssi_transfer(s->spi, 0);
        }
    }
    if (user & (USER_MOSI | USER_MISO)) {
        unsigned bits = (s->regs[R_MS_DLEN >> 2] & 0x3ffff) + 1;
        unsigned bytes = MIN((bits + 7) / 8, GPSPI_FIFO_WORDS * 4);
        for (unsigned i = 0; i < bytes; ++i) {
            uint8_t tx = (user & USER_MOSI) ? gpspi_fifo_get(s, i) : 0xff;
            uint8_t rx = ssi_transfer(s->spi, tx);
            if (user & USER_MISO) {
                gpspi_fifo_set(s, i, rx);
            }
        }
    }
    s->regs[R_CMD >> 2] &= ~CMD_USR;
    s->regs[R_DMA_INT_RAW >> 2] |= INT_TRANS_DONE;
    gpspi_update_irq(s);
}

static uint64_t gpspi_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32S3GpSpiState *s = ESP32S3_GPSPI(opaque);
    if (size != 4 || (addr & 3) || addr >= GPSPI_MMIO_SIZE) {
        return 0;
    }
    if (addr == R_DMA_INT_ST) {
        return s->regs[R_DMA_INT_RAW >> 2] & s->regs[R_DMA_INT_ENA >> 2];
    }
    return *gpspi_reg(s, addr);
}

static void gpspi_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32S3GpSpiState *s = ESP32S3_GPSPI(opaque);
    if (size != 4 || (addr & 3) || addr >= GPSPI_MMIO_SIZE) {
        return;
    }
    uint32_t v = value;
    switch (addr) {
    case R_CMD:
        s->regs[R_CMD >> 2] = v & ~(CMD_UPDATE | CMD_USR);
        if (v & CMD_USR) {
            gpspi_transaction(s);
        }
        break;
    case R_DMA_INT_ENA:
        s->regs[R_DMA_INT_ENA >> 2] = v;
        gpspi_update_irq(s);
        break;
    case R_DMA_INT_CLR:
        s->regs[R_DMA_INT_RAW >> 2] &= ~v;
        gpspi_update_irq(s);
        break;
    case R_DMA_INT_SET:
        s->regs[R_DMA_INT_RAW >> 2] |= v;
        gpspi_update_irq(s);
        break;
    default:
        *gpspi_reg(s, addr) = v;
        break;
    }
}

static const MemoryRegionOps gpspi_ops = {
    .read = gpspi_read,
    .write = gpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gpspi_reset(DeviceState *dev)
{
    ESP32S3GpSpiState *s = ESP32S3_GPSPI(dev);
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void gpspi_init(Object *obj)
{
    ESP32S3GpSpiState *s = ESP32S3_GPSPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &gpspi_ops, s, TYPE_ESP32S3_GPSPI, GPSPI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->spi = ssi_create_bus(DEVICE(obj), "spi");
}

static void gpspi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;
    dc->legacy_reset = gpspi_reset;
}

static const TypeInfo gpspi_info = {
    .name = TYPE_ESP32S3_GPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3GpSpiState),
    .instance_init = gpspi_init,
    .class_init = gpspi_class_init,
};

static void gpspi_register_types(void)
{
    type_register_static(&gpspi_info);
}
type_init(gpspi_register_types)
'''


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        return 2
    root = Path(argv[1]).resolve()
    source = root / "hw/ssi/esp32s3_gpspi.c"
    if source.exists():
        raise RuntimeError(f"refusing to overwrite upstream file: {source}")
    source.write_text(GPSPI_SOURCE, encoding="utf-8")

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
    text = replace_once(text,
        '#define TYPE_MURPHY_FT6X36 "murphy-ft6x36"\n',
        '#define TYPE_MURPHY_FT6X36 "murphy-ft6x36"\n#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"\n',
        "define GP-SPI2 type")
    text = replace_once(text,
        "    SsiPsramState *psram;\n\n    uint32_t sens_regs",
        "    SsiPsramState *psram;\n    DeviceState *gpspi2;\n\n    uint32_t sens_regs",
        "add GP-SPI2 state")
    text = replace_once(text,
        "    object_initialize_child(OBJECT(ss), \"spi1\", &ss->spi1, TYPE_ESP32S3_SPI);",
        "    object_initialize_child(OBJECT(ss), \"spi1\", &ss->spi1, TYPE_ESP32S3_SPI);\n"
        "    ss->gpspi2 = qdev_new(TYPE_ESP32S3_GPSPI);\n"
        "    object_property_add_child(OBJECT(ss), \"gpspi2\", OBJECT(ss->gpspi2));",
        "create GP-SPI2 child")
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

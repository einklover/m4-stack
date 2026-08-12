#!/usr/bin/env python3
"""Attach a digital SSD1677 e-paper controller to Murphy M4 SPI2/GPIO."""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one upstream match, found {count}")
    return text.replace(old, new, 1)


SOURCE = r'''#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-core.h"
#include "qemu/module.h"

#define TYPE_MURPHY_SSD1677 "murphy-ssd1677"
OBJECT_DECLARE_SIMPLE_TYPE(MurphySsd1677State, MURPHY_SSD1677)
#define EPD_W 800
#define EPD_H 480
#define EPD_ROW_BYTES (EPD_W / 8)
#define EPD_FB_BYTES (EPD_ROW_BYTES * EPD_H)
#define DIRECT_FRAME_BASE 0x60100000
#define DIRECT_REFRESH_BASE 0x6010c000
#define CMD_DEEP_SLEEP 0x10
#define CMD_DATA_ENTRY 0x11
#define CMD_SOFT_RESET 0x12
#define CMD_MASTER_ACT 0x20
#define CMD_UPDATE_CTRL1 0x21
#define CMD_UPDATE_CTRL2 0x22
#define CMD_WRITE_BW 0x24
#define CMD_WRITE_RED 0x26
#define CMD_SET_X_RANGE 0x44
#define CMD_SET_Y_RANGE 0x45
#define CMD_SET_X_COUNTER 0x4e
#define CMD_SET_Y_COUNTER 0x4f

struct MurphySsd1677State {
    SSIPeripheral parent_obj;
    qemu_irq busy;
    QEMUTimer *busy_timer;
    bool dc;
    bool reset_level;
    bool busy_level;
    bool sleeping;
    uint8_t command;
    uint8_t update_ctrl1;
    uint8_t update_ctrl2;
    uint8_t data_entry;
    uint8_t params[128];
    unsigned expected;
    unsigned received;
    uint16_t x_start, x_end, y_start, y_end;
    size_t ram_received;
    uint8_t bw_ram[EPD_FB_BYTES];
    uint8_t red_ram[EPD_FB_BYTES];
    uint8_t physical[EPD_FB_BYTES];
    char *frame_file;
    uint32_t busy_ms;
    MemoryRegion direct_ram;
    MemoryRegion direct_refresh;
};

static unsigned expected_params(uint8_t cmd)
{
    switch (cmd) {
    case CMD_DATA_ENTRY:
    case CMD_UPDATE_CTRL1:
    case CMD_UPDATE_CTRL2:
    case CMD_DEEP_SLEEP: return 1;
    case CMD_SET_X_RANGE:
    case CMD_SET_Y_RANGE: return 4;
    case CMD_SET_X_COUNTER:
    case CMD_SET_Y_COUNTER: return 2;
    default: return 0;
    }
}

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

static void set_busy(MurphySsd1677State *s, bool level)
{
    s->busy_level = level;
    qemu_set_irq(s->busy, level);
}

static void export_frame(MurphySsd1677State *s)
{
    if (!s->frame_file || !*s->frame_file) return;
    char *tmp = g_strdup_printf("%s.tmp", s->frame_file);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        warn_report("murphy-ssd1677: cannot open frame file %s", s->frame_file);
        g_free(tmp);
        return;
    }
    fprintf(f, "P4\n%d %d\n", EPD_W, EPD_H);
    for (size_t i = 0; i < EPD_FB_BYTES; ++i) {
        uint8_t b = ~s->physical[i];
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    if (rename(tmp, s->frame_file) != 0) {
        warn_report("murphy-ssd1677: cannot publish frame file %s", s->frame_file);
    }
    g_free(tmp);
}

static uint64_t direct_refresh_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque; (void)addr; (void)size; return 0;
}

static void direct_refresh_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    MurphySsd1677State *s = MURPHY_SSD1677(opaque);
    (void)addr; (void)value; (void)size;
    memcpy(s->physical, memory_region_get_ram_ptr(&s->direct_ram), EPD_FB_BYTES);
    export_frame(s);
}

static const MemoryRegionOps direct_refresh_ops = {
    .read = direct_refresh_read,
    .write = direct_refresh_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void reset_state(MurphySsd1677State *s, bool preserve_physical)
{
    if (s->busy_timer) timer_del(s->busy_timer);
    set_busy(s, false);
    s->sleeping = false;
    s->command = 0;
    s->expected = s->received = 0;
    s->data_entry = 1;
    s->ram_received = 0;
    s->x_start = 0; s->x_end = EPD_W - 1;
    s->y_start = 0; s->y_end = EPD_H - 1;
    memset(s->bw_ram, 0xff, sizeof(s->bw_ram));
    memset(s->red_ram, 0xff, sizeof(s->red_ram));
    if (!preserve_physical) memset(s->physical, 0xff, sizeof(s->physical));
}

static void busy_done(void *opaque)
{
    MurphySsd1677State *s = MURPHY_SSD1677(opaque);
    memcpy(s->physical, s->bw_ram, sizeof(s->physical));
    export_frame(s);
    set_busy(s, false);
}

static void activate(MurphySsd1677State *s)
{
    if (s->sleeping || s->busy_level) return;
    set_busy(s, true);
    timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + MAX(1u, s->busy_ms));
}

static void apply_params(MurphySsd1677State *s)
{
    switch (s->command) {
    case CMD_DATA_ENTRY: s->data_entry = s->params[0]; break;
    case CMD_SET_X_RANGE: s->x_start = le16(s->params); s->x_end = le16(s->params + 2); break;
    case CMD_SET_Y_RANGE: s->y_start = le16(s->params); s->y_end = le16(s->params + 2); break;
    case CMD_UPDATE_CTRL1: s->update_ctrl1 = s->params[0]; break;
    case CMD_UPDATE_CTRL2: s->update_ctrl2 = s->params[0]; break;
    case CMD_DEEP_SLEEP: s->sleeping = true; break;
    default: break;
    }
}

static size_t window_bytes(MurphySsd1677State *s)
{
    uint16_t xlo = MIN(s->x_start, s->x_end), xhi = MAX(s->x_start, s->x_end);
    uint16_t ylo = MIN(s->y_start, s->y_end), yhi = MAX(s->y_start, s->y_end);
    if (xhi >= EPD_W || yhi >= EPD_H || ((xhi - xlo + 1) & 7)) return 0;
    return ((xhi - xlo + 1) / 8) * (size_t)(yhi - ylo + 1);
}

static void ram_byte(MurphySsd1677State *s, uint8_t value)
{
    size_t total = window_bytes(s);
    if (!total || s->ram_received >= total) return;
    uint16_t xlo = MIN(s->x_start, s->x_end), xhi = MAX(s->x_start, s->x_end);
    uint16_t ylo = MIN(s->y_start, s->y_end);
    size_t row_bytes = (xhi - xlo + 1) / 8;
    size_t row = s->ram_received / row_bytes, col = s->ram_received % row_bytes;
    size_t dst = ((size_t)ylo + row) * EPD_ROW_BYTES + xlo / 8 + col;
    if (dst < EPD_FB_BYTES) {
        (s->command == CMD_WRITE_BW ? s->bw_ram : s->red_ram)[dst] = value;
    }
    ++s->ram_received;
}

static void command(MurphySsd1677State *s, uint8_t cmd)
{
    s->command = cmd;
    s->expected = expected_params(cmd);
    s->received = 0;
    s->ram_received = 0;
    if (cmd == CMD_SOFT_RESET) {
        reset_state(s, true);
        set_busy(s, true);
        timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
    } else if (cmd == CMD_MASTER_ACT) {
        activate(s);
    }
}

static uint32_t transfer(SSIPeripheral *dev, uint32_t value)
{
    MurphySsd1677State *s = MURPHY_SSD1677(dev);
    uint8_t b = value;
    if (!s->reset_level) return 0xff;
    if (!s->dc) {
        command(s, b);
        return 0xff;
    }
    if (s->command == CMD_WRITE_BW || s->command == CMD_WRITE_RED) {
        ram_byte(s, b);
        return 0xff;
    }
    if (s->received < sizeof(s->params)) s->params[s->received] = b;
    ++s->received;
    if (s->expected && s->received == s->expected) apply_params(s);
    return 0xff;
}

static void set_dc(void *opaque, int n, int level)
{
    MurphySsd1677State *s = MURPHY_SSD1677(opaque); (void)n; s->dc = level != 0;
}

static void set_reset(void *opaque, int n, int level)
{
    MurphySsd1677State *s = MURPHY_SSD1677(opaque); (void)n;
    bool next = level != 0;
    if (s->reset_level && !next) reset_state(s, true);
    s->reset_level = next;
}

static int set_cs(SSIPeripheral *dev, bool select)
{
    (void)dev; (void)select; return 0;
}

static void device_reset(DeviceState *dev)
{
    MurphySsd1677State *s = MURPHY_SSD1677(dev);
    s->reset_level = true;
    reset_state(s, false);
}

static void realize(SSIPeripheral *dev, Error **errp)
{
    MurphySsd1677State *s = MURPHY_SSD1677(dev); (void)errp;
    qdev_init_gpio_in_named(DEVICE(dev), set_dc, "dc", 1);
    qdev_init_gpio_in_named(DEVICE(dev), set_reset, "reset", 1);
    qdev_init_gpio_out_named(DEVICE(dev), &s->busy, "busy", 1);
    s->busy_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, busy_done, s);
    memory_region_init_ram(&s->direct_ram, OBJECT(s), "murphy-ssd1677.direct-frame",
                           EPD_FB_BYTES, &error_fatal);
    memory_region_add_subregion(get_system_memory(), DIRECT_FRAME_BASE, &s->direct_ram);
    memory_region_init_io(&s->direct_refresh, OBJECT(s), &direct_refresh_ops, s,
                          "murphy-ssd1677.direct-refresh", 4);
    memory_region_add_subregion(get_system_memory(), DIRECT_REFRESH_BASE, &s->direct_refresh);
    device_reset(DEVICE(dev));
}

static Property props[] = {
    DEFINE_PROP_STRING("frame-file", MurphySsd1677State, frame_file),
    DEFINE_PROP_UINT32("busy-ms", MurphySsd1677State, busy_ms, 40),
    DEFINE_PROP_END_OF_LIST(),
};

static void class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass); (void)data;
    dc->legacy_reset = device_reset;
    device_class_set_props(dc, props);
    sc->realize = realize;
    sc->transfer = transfer;
    sc->set_cs = set_cs;
    sc->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo info = {
    .name = TYPE_MURPHY_SSD1677,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(MurphySsd1677State),
    .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
'''


def main(argv: list[str]) -> int:
    if len(argv) != 2: return 2
    root = Path(argv[1]).resolve()
    source = root / "hw/ssi/murphy_ssd1677.c"
    if source.exists(): raise RuntimeError(f"refusing to overwrite upstream file: {source}")
    source.write_text(SOURCE, encoding="utf-8")

    meson = root / "hw/ssi/meson.build"
    m = meson.read_text(encoding="utf-8")
    m = replace_once(m,
        "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c', 'esp32s3_gpspi.c'))",
        "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c', 'esp32s3_gpspi.c', 'murphy_ssd1677.c'))",
        "compile SSD1677")
    meson.write_text(m, encoding="utf-8")

    path = root / "hw/xtensa/esp32s3.c"
    text = path.read_text(encoding="utf-8")
    # SSIBus / ssi_realize_and_unref / SSI_GPIO_CS live in hw/ssi/ssi.h.
    if '#include "hw/ssi/ssi.h"' not in text:
        text = replace_once(
            text,
            '#include "hw/ssi/esp32s3_spi.h"\n',
            '#include "hw/ssi/esp32s3_spi.h"\n#include "hw/ssi/ssi.h"\n',
            "include ssi.h for SSD1677 attach",
        )
    text = replace_once(text,
        '#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"\n',
        '#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"\n#define TYPE_MURPHY_SSD1677 "murphy-ssd1677"\n',
        "define SSD1677 type")
    text = replace_once(text,
        "    DeviceState *flash_dev;\n    DeviceState *touch_dev;\n};",
        "    DeviceState *flash_dev;\n    DeviceState *touch_dev;\n    DeviceState *epd_dev;\n};",
        "retain SSD1677")
    anchor = """        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 45,
                                    qdev_get_gpio_in_named(ms->touch_dev, "power", 0));
    }
"""
    replacement = """        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 45,
                                    qdev_get_gpio_in_named(ms->touch_dev, "power", 0));

        SSIBus *spi_bus = (SSIBus *)qdev_get_child_bus(ss->gpspi2, "spi");
        DeviceState *epd = qdev_new(TYPE_MURPHY_SSD1677);
        ms->epd_dev = epd;
        ssi_realize_and_unref(epd, spi_bus, &error_fatal);
        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 5,
                                    qdev_get_gpio_in_named(epd, SSI_GPIO_CS, 0));
        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 6,
                                    qdev_get_gpio_in_named(epd, "dc", 0));
        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 7,
                                    qdev_get_gpio_in_named(epd, "reset", 0));
        qdev_connect_gpio_out_named(epd, "busy", 0,
                                    qdev_get_gpio_in_named(DEVICE(&ss->gpio), "gpio-in", 8));
    }
"""
    text = replace_once(text, anchor, replacement, "attach SSD1677")
    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

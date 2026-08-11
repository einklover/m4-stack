#!/usr/bin/env python3
"""Attach a functional SSD1677 controller to Murphy M4 SPI2/GPIO."""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one upstream match, found {count}")
    return text.replace(old, new, 1)


SSD1677_SOURCE = r'''/* Murphy M4 SSD1677 digital e-paper controller model.
 *
 * This models the production-firmware-visible boundary: SPI bytes gated by
 * external CS, GPIO DC/RST/BUSY, controller RAM/window registers and atomic
 * refresh completion. It intentionally does not model electrophoretic analog
 * waveforms or ghosting.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"

#define TYPE_MURPHY_SSD1677 "murphy-ssd1677"
OBJECT_DECLARE_SIMPLE_TYPE(MurphySsd1677State, MURPHY_SSD1677)

#define EPD_W 800
#define EPD_H 480
#define EPD_ROW_BYTES (EPD_W / 8)
#define EPD_FB_BYTES (EPD_ROW_BYTES * EPD_H)

#define CMD_DRIVER_OUTPUT   0x01
#define CMD_BOOSTER         0x0c
#define CMD_DEEP_SLEEP      0x10
#define CMD_DATA_ENTRY      0x11
#define CMD_SOFT_RESET      0x12
#define CMD_TEMP_SENSOR     0x18
#define CMD_WRITE_TEMP      0x1a
#define CMD_MASTER_ACT      0x20
#define CMD_UPDATE_CTRL1    0x21
#define CMD_UPDATE_CTRL2    0x22
#define CMD_WRITE_BW        0x24
#define CMD_WRITE_RED       0x26
#define CMD_WRITE_VCOM      0x2c
#define CMD_WRITE_LUT       0x32
#define CMD_BORDER          0x3c
#define CMD_SET_X_RANGE     0x44
#define CMD_SET_Y_RANGE     0x45
#define CMD_AUTO_BW         0x46
#define CMD_AUTO_RED        0x47
#define CMD_SET_X_COUNTER   0x4e
#define CMD_SET_Y_COUNTER   0x4f
#define CMD_GATE_VOLTAGE    0x03
#define CMD_SOURCE_VOLTAGE  0x04

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
    uint8_t command_data[128];
    unsigned expected;
    unsigned received;
    uint16_t x_start;
    uint16_t x_end;
    uint16_t y_start;
    uint16_t y_end;
    uint16_t x_counter;
    uint16_t y_counter;
    size_t ram_received;
    uint8_t bw_ram[EPD_FB_BYTES];
    uint8_t red_ram[EPD_FB_BYTES];
    uint8_t physical[EPD_FB_BYTES];
    char *frame_file;
    uint32_t busy_ms;
    uint64_t activations;
};

static unsigned ssd1677_expected(uint8_t cmd)
{
    switch (cmd) {
    case CMD_BOOSTER: return 5;
    case CMD_DRIVER_OUTPUT: return 3;
    case CMD_BORDER:
    case CMD_TEMP_SENSOR:
    case CMD_DATA_ENTRY:
    case CMD_AUTO_BW:
    case CMD_AUTO_RED:
    case CMD_UPDATE_CTRL1:
    case CMD_UPDATE_CTRL2:
    case CMD_GATE_VOLTAGE:
    case CMD_WRITE_VCOM:
    case CMD_WRITE_TEMP:
    case CMD_DEEP_SLEEP: return 1;
    case CMD_SET_X_RANGE:
    case CMD_SET_Y_RANGE: return 4;
    case CMD_SET_X_COUNTER:
    case CMD_SET_Y_COUNTER: return 2;
    case CMD_WRITE_LUT: return 105;
    case CMD_SOURCE_VOLTAGE: return 3;
    default: return 0;
    }
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void ssd1677_set_busy(MurphySsd1677State *s, bool busy)
{
    s->busy_level = busy;
    qemu_set_irq(s->busy, busy ? 1 : 0);
}

static void ssd1677_reset_state(MurphySsd1677State *s, bool preserve_physical)
{
    timer_del(s->busy_timer);
    ssd1677_set_busy(s, false);
    s->sleeping = false;
    s->command = 0;
    s->update_ctrl1 = 0;
    s->update_ctrl2 = 0;
    s->data_entry = 1;
    s->expected = 0;
    s->received = 0;
    s->ram_received = 0;
    s->x_start = 0;
    s->x_end = EPD_W - 1;
    s->y_start = EPD_H - 1;
    s->y_end = 0;
    s->x_counter = s->x_start;
    s->y_counter = s->y_start;
    memset(s->bw_ram, 0xff, sizeof(s->bw_ram));
    memset(s->red_ram, 0xff, sizeof(s->red_ram));
    if (!preserve_physical) {
        memset(s->physical, 0xff, sizeof(s->physical));
    }
}

static void ssd1677_export_frame(MurphySsd1677State *s)
{
    if (!s->frame_file || !*s->frame_file) {
        return;
    }
    FILE *f = fopen(s->frame_file, "wb");
    if (!f) {
        warn_report("murphy-ssd1677: cannot open frame-file '%s'", s->frame_file);
        return;
    }
    fprintf(f, "P4\n%d %d\n", EPD_W, EPD_H);
    /* SSD RAM uses 1=white, PBM P4 uses 1=black. */
    for (size_t i = 0; i < sizeof(s->physical); ++i) {
        uint8_t b = ~s->physical[i];
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
}

static void ssd1677_busy_done(void *opaque)
{
    MurphySsd1677State *s = MURPHY_SSD1677(opaque);
    memcpy(s->physical, s->bw_ram, sizeof(s->physical));
    ++s->activations;
    ssd1677_export_frame(s);
    ssd1677_set_busy(s, false);
}

static void ssd1677_activate(MurphySsd1677State *s)
{
    if (s->sleeping || s->busy_level) {
        return;
    }
    ssd1677_set_busy(s, true);
    timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + MAX(1u, s->busy_ms));
}

static void ssd1677_apply_register(MurphySsd1677State *s)
{
    switch (s->command) {
    case CMD_DATA_ENTRY: s->data_entry = s->command_data[0]; break;
    case CMD_SET_X_RANGE:
        s->x_start = le16(&s->command_data[0]);
        s->x_end = le16(&s->command_data[2]);
        break;
    case CMD_SET_Y_RANGE:
        s->y_start = le16(&s->command_data[0]);
        s->y_end = le16(&s->command_data[2]);
        break;
    case CMD_SET_X_COUNTER: s->x_counter = le16(&s->command_data[0]); break;
    case CMD_SET_Y_COUNTER: s->y_counter = le16(&s->command_data[0]); break;
    case CMD_UPDATE_CTRL1: s->update_ctrl1 = s->command_data[0]; break;
    case CMD_UPDATE_CTRL2: s->update_ctrl2 = s->command_data[0]; break;
    case CMD_DEEP_SLEEP: s->sleeping = true; break;
    default: break;
    }
}

static size_t ssd1677_window_bytes(MurphySsd1677State *s)
{
    uint16_t xlo = MIN(s->x_start, s->x_end);
    uint16_t xhi = MAX(s->x_start, s->x_end);
    uint16_t ylo = MIN(s->y_start, s->y_end);
    uint16_t yhi = MAX(s->y_start, s->y_end);
    if (xhi >= EPD_W || yhi >= EPD_H || xlo > xhi || ylo > yhi) {
        return 0;
    }
    unsigned width = xhi - xlo + 1;
    if (width & 7) {
        return 0;
    }
    return (size_t)(width / 8) * (yhi - ylo + 1);
}

static void ssd1677_ram_byte(MurphySsd1677State *s, uint8_t value)
{
    const size_t expected = ssd1677_window_bytes(s);
    if (!expected || s->ram_received >= expected) {
        return;
    }
    const uint16_t xlo = MIN(s->x_start, s->x_end);
    const uint16_t xhi = MAX(s->x_start, s->x_end);
    const uint16_t ylo = MIN(s->y_start, s->y_end);
    const size_t row_bytes = (xhi - xlo + 1) / 8;
    const size_t row = s->ram_received / row_bytes;
    const size_t col = s->ram_received % row_bytes;
    const size_t dst = ((size_t)ylo + row) * EPD_ROW_BYTES + xlo / 8 + col;
    if (dst < EPD_FB_BYTES) {
        if (s->command == CMD_WRITE_BW) {
            s->bw_ram[dst] = value;
        } else {
            s->red_ram[dst] = value;
        }
    }
    ++s->ram_received;
}

static void ssd1677_command(MurphySsd1677State *s, uint8_t cmd)
{
    s->command = cmd;
    s->expected = ssd1677_expected(cmd);
    s->received = 0;
    s->ram_received = 0;
    if (cmd == CMD_SOFT_RESET) {
        ssd1677_reset_state(s, true);
        ssd1677_set_busy(s, true);
        timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
    } else if (cmd == CMD_MASTER_ACT) {
        ssd1677_activate(s);
    }
}

static uint32_t ssd1677_transfer(SSIPeripheral *dev, uint32_t value)
{
    MurphySsd1677State *s = MURPHY_SSD1677(dev);
    const uint8_t byte = value;
    if (!s->reset_level) {
        return 0xff;
    }
    if (!s->dc) {
        ssd1677_command(s, byte);
        return 0xff;
    }

    if (s->command == CMD_WRITE_BW || s->command == CMD_WRITE_RED) {
        ssd1677_ram_byte(s, byte);
        return 0xff;
    }
    if (s->received < sizeof(s->command_data)) {
        s->command_data[s->received] = byte;
    }
    ++s->received;
    if (s->expected && s->received == s->expected) {
        ssd1677_apply_register(s);
    }
    return 0xff;
}

static void ssd1677_set_dc(void *opaque, int n, int level)
{
    MurphySsd1677State *s = MURPHY_SSD1677(opaque);
    (void)n;
    s->dc = level != 0;
}

static void ssd1677_set_reset(void *opaque, int n, int level)
{
    MurphySsd1677State *s = MURPHY_SSD1677(opaque);
    (void)n;
    bool next = level != 0;
    if (s->reset_level && !next) {
        ssd1677_reset_state(s, true);
    }
    s->reset_level = next;
}

static int ssd1677_set_cs(SSIPeripheral *dev, bool select)
{
    MurphySsd1677State *s = MURPHY_SSD1677(dev);
    if (!select) {
        /* A new transaction may continue RAM data; only the SPI framing ends. */
        s->received = MIN(s->received, s->expected);
    }
    return 0;
}

static void ssd1677_reset(DeviceState *dev)
{
    MurphySsd1677State *s = MURPHY_SSD1677(dev);
    s->reset_level = true;
    ssd1677_reset_state(s, false);
}

static void ssd1677_realize(SSIPeripheral *dev, Error **errp)
{
    MurphySsd1677State *s = MURPHY_SSD1677(dev);
    (void)errp;
    qdev_init_gpio_in_named(DEVICE(dev), ssd1677_set_dc, "dc", 1);
    qdev_init_gpio_in_named(DEVICE(dev), ssd1677_set_reset, "reset", 1);
    qdev_init_gpio_out_named(DEVICE(dev), &s->busy, "busy", 1);
    s->busy_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ssd1677_busy_done, s);
    ssd1677_reset(DEVICE(dev));
}

static Property ssd1677_properties[] = {
    DEFINE_PROP_STRING("frame-file", MurphySsd1677State, frame_file),
    DEFINE_PROP_UINT32("busy-ms", MurphySsd1677State, busy_ms, 40),
    DEFINE_PROP_END_OF_LIST(),
};

static void ssd1677_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);
    (void)data;
    dc->legacy_reset = ssd1677_reset;
    device_class_set_props(dc, ssd1677_properties);
    sc->realize = ssd1677_realize;
    sc->transfer = ssd1677_transfer;
    sc->set_cs = ssd1677_set_cs;
    sc->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo ssd1677_info = {
    .name = TYPE_MURPHY_SSD1677,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(MurphySsd1677State),
    .class_init = ssd1677_class_init,
};

static void ssd1677_register_types(void)
{
    type_register_static(&ssd1677_info);
}

type_init(ssd1677_register_types)
'''


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} QEMU_SOURCE", file=sys.stderr)
        return 2
    root = Path(argv[1]).resolve()

    source = root / "hw/ssi/murphy_ssd1677.c"
    if source.exists():
        raise RuntimeError(f"refusing to overwrite upstream file: {source}")
    source.write_text(SSD1677_SOURCE, encoding="utf-8")

    meson = root / "hw/ssi/meson.build"
    mtext = meson.read_text(encoding="utf-8")
    mtext = replace_once(
        mtext,
        "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c', 'esp32s3_gpspi.c'))",
        "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c', 'esp32s3_gpspi.c', 'murphy_ssd1677.c'))",
        "compile Murphy SSD1677 device",
    )
    meson.write_text(mtext, encoding="utf-8")

    path = root / "hw/xtensa/esp32s3.c"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"\n',
        '#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"\n#define TYPE_MURPHY_SSD1677 "murphy-ssd1677"\n',
        "define SSD1677 device type",
    )
    text = replace_once(
        text,
        "    DeviceState *flash_dev;\n    DeviceState *touch_dev;\n};",
        "    DeviceState *flash_dev;\n    DeviceState *touch_dev;\n    DeviceState *epd_dev;\n};",
        "retain SSD1677 device in Murphy machine state",
    )

    touch_tail = """        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 45,
                                    qdev_get_gpio_in_named(ms->touch_dev, "power", 0));
    }
"""
    epd_tail = """        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 45,
                                    qdev_get_gpio_in_named(ms->touch_dev, "power", 0));

        SSIBus *spi_bus = SSI_BUS(qdev_get_child_bus(ss->gpspi2, "spi"));
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
    text = replace_once(text, touch_tail, epd_tail, "attach SSD1677 to Murphy SPI2/GPIO")
    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Add the Murphy M4 machine type and its FT6x36-compatible touch device."""
from __future__ import annotations

from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one upstream match, found {count}")
    return text.replace(old, new, 1)


TOUCH_SOURCE = r'''/* Murphy M4 FT6x36-compatible touch controller.
 *
 * The production driver selects register 0, terminates the write, waits, then
 * reads an 8-byte frame.  Keep this device intentionally small and digital:
 * I2C register/frame semantics plus active-low IRQ and active-low board power.
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"

#define TYPE_MURPHY_FT6X36 "murphy-ft6x36"
OBJECT_DECLARE_SIMPLE_TYPE(MurphyFt6x36State, MURPHY_FT6X36)

typedef struct MurphyFt6x36State {
    I2CSlave parent_obj;
    qemu_irq irq;
    bool powered;
    bool pressed;
    uint16_t x;
    uint16_t y;
    uint8_t selected_reg;
    uint8_t read_index;
    uint8_t write_index;
} MurphyFt6x36State;

static void murphy_ft6x36_update_irq(MurphyFt6x36State *s)
{
    /* Physical TOUCH_INT is idle high and asserted low for a pending touch. */
    qemu_set_irq(s->irq, (s->powered && s->pressed) ? 0 : 1);
}

static void murphy_ft6x36_set_power(void *opaque, int n, int level)
{
    MurphyFt6x36State *s = MURPHY_FT6X36(opaque);
    (void)n;
    /* Murphy TOUCH_PWR is active-low in the verified board contract. */
    s->powered = !level;
    s->read_index = 0;
    s->write_index = 0;
    murphy_ft6x36_update_irq(s);
}

static uint8_t murphy_ft6x36_frame_byte(MurphyFt6x36State *s, unsigned index)
{
    const uint16_t x = MIN(s->x, 479);
    const uint16_t y = MIN(s->y, 799);
    switch (index) {
    case 0: return 0;
    case 1: return 0;
    case 2: return s->pressed ? 1 : 0;
    case 3:
        return s->pressed ? (uint8_t)(0x80 | ((x >> 8) & 0x0f)) : 0;
    case 4: return s->pressed ? (uint8_t)x : 0;
    case 5: return s->pressed ? (uint8_t)((y >> 8) & 0x0f) : 0;
    case 6: return s->pressed ? (uint8_t)y : 0;
    case 7: return 0;
    default: return 0;
    }
}

static int murphy_ft6x36_event(I2CSlave *i2c, enum i2c_event event)
{
    MurphyFt6x36State *s = MURPHY_FT6X36(i2c);
    if (!s->powered && (event == I2C_START_SEND || event == I2C_START_RECV)) {
        return 1;
    }
    if (event == I2C_START_SEND) {
        s->write_index = 0;
    } else if (event == I2C_START_RECV) {
        s->read_index = 0;
    }
    return 0;
}

static int murphy_ft6x36_send(I2CSlave *i2c, uint8_t data)
{
    MurphyFt6x36State *s = MURPHY_FT6X36(i2c);
    if (!s->powered) {
        return -1;
    }
    if (s->write_index == 0) {
        s->selected_reg = data;
    }
    ++s->write_index;
    return 0;
}

static uint8_t murphy_ft6x36_recv(I2CSlave *i2c)
{
    MurphyFt6x36State *s = MURPHY_FT6X36(i2c);
    if (!s->powered || s->selected_reg != 0) {
        return 0xff;
    }
    return murphy_ft6x36_frame_byte(s, s->read_index++);
}

static void murphy_ft6x36_reset(DeviceState *dev)
{
    MurphyFt6x36State *s = MURPHY_FT6X36(dev);
    s->selected_reg = 0;
    s->read_index = 0;
    s->write_index = 0;
    murphy_ft6x36_update_irq(s);
}

static void murphy_ft6x36_realize(DeviceState *dev, Error **errp)
{
    MurphyFt6x36State *s = MURPHY_FT6X36(dev);
    (void)errp;
    qdev_init_gpio_out_named(dev, &s->irq, "irq", 1);
    qdev_init_gpio_in_named(dev, murphy_ft6x36_set_power, "power", 1);
    murphy_ft6x36_reset(dev);
}

static Property murphy_ft6x36_properties[] = {
    DEFINE_PROP_BOOL("powered", MurphyFt6x36State, powered, true),
    DEFINE_PROP_BOOL("pressed", MurphyFt6x36State, pressed, false),
    DEFINE_PROP_UINT16("x", MurphyFt6x36State, x, 240),
    DEFINE_PROP_UINT16("y", MurphyFt6x36State, y, 400),
    DEFINE_PROP_END_OF_LIST(),
};

static void murphy_ft6x36_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    (void)data;
    dc->realize = murphy_ft6x36_realize;
    dc->reset = murphy_ft6x36_reset;
    device_class_set_props(dc, murphy_ft6x36_properties);
    sc->event = murphy_ft6x36_event;
    sc->send = murphy_ft6x36_send;
    sc->recv = murphy_ft6x36_recv;
}

static const TypeInfo murphy_ft6x36_info = {
    .name = TYPE_MURPHY_FT6X36,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(MurphyFt6x36State),
    .class_init = murphy_ft6x36_class_init,
};

static void murphy_ft6x36_register_types(void)
{
    type_register_static(&murphy_ft6x36_info);
}

type_init(murphy_ft6x36_register_types)
'''


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} QEMU_SOURCE", file=sys.stderr)
        return 2
    root = Path(argv[1]).resolve()

    touch_path = root / "hw/input/murphy_ft6x36.c"
    if touch_path.exists():
        raise RuntimeError(f"refusing to overwrite upstream file: {touch_path}")
    touch_path.write_text(TOUCH_SOURCE, encoding="utf-8")

    meson = root / "hw/input/meson.build"
    meson_text = meson.read_text(encoding="utf-8")
    meson_text = replace_once(
        meson_text,
        "system_ss.add(files('hid.c'))\n",
        "system_ss.add(files('hid.c'))\n"
        "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('murphy_ft6x36.c'))\n",
        "add Murphy touch source to ESP32-S3 build",
    )
    meson.write_text(meson_text, encoding="utf-8")

    path = root / "hw/xtensa/esp32s3.c"
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#define TYPE_ESP32S3_MACHINE MACHINE_TYPE_NAME("esp32s3")\n',
        '#define TYPE_ESP32S3_MACHINE MACHINE_TYPE_NAME("esp32s3")\n'
        '#define TYPE_MURPHY_M4_MACHINE MACHINE_TYPE_NAME("murphy-m4")\n'
        '#define TYPE_MURPHY_FT6X36 "murphy-ft6x36"\n',
        "define Murphy machine and touch types",
    )

    text = replace_once(
        text,
        "    Esp32s3SocState esp32s3;\n    DeviceState *flash_dev;\n};",
        "    Esp32s3SocState esp32s3;\n    DeviceState *flash_dev;\n    DeviceState *touch_dev;\n};",
        "retain Murphy touch device in machine state",
    )

    gpio_block = """    /* GPIO realization */
    {
        sysbus_realize(SYS_BUS_DEVICE(&ss->gpio), &error_fatal);
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->gpio), 0);
        memory_region_add_subregion_overlap(sys_mem, DR_REG_GPIO_BASE, mr, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(&ss->gpio), 0,
                           qdev_get_gpio_in(intmatrix_dev, ETS_GPIO_INTR_SOURCE));
    }
"""
    board_block = gpio_block + r'''
    if (object_dynamic_cast(OBJECT(machine), TYPE_MURPHY_M4_MACHINE)) {
        I2CBus *i2c_bus = I2C_BUS(qdev_get_child_bus(DEVICE(&ss->i2c[0]), "i2c"));
        I2CSlave *touch = i2c_slave_create_simple(i2c_bus, TYPE_MURPHY_FT6X36, 0x2e);
        ms->touch_dev = DEVICE(touch);
        qdev_connect_gpio_out_named(ms->touch_dev, "irq", 0,
                                    qdev_get_gpio_in_named(DEVICE(&ss->gpio), "gpio-in", 44));
        qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 45,
                                    qdev_get_gpio_in_named(ms->touch_dev, "power", 0));
    }
'''
    text = replace_once(text, gpio_block, board_block, "attach Murphy touch after GPIO realization")

    old_tail = r'''static const TypeInfo esp32s3_info = {
    .name = TYPE_ESP32S3_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Esp32s3MachineState),
    .class_init = esp32s3_machine_class_init,
};

static void esp32s3_machine_type_init(void)
{
    type_register_static(&esp32s3_info);
}

type_init(esp32s3_machine_type_init);
'''
    new_tail = r'''static const TypeInfo esp32s3_info = {
    .name = TYPE_ESP32S3_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Esp32s3MachineState),
    .class_init = esp32s3_machine_class_init,
};

static void murphy_m4_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    (void)data;
    mc->desc = "Murphy M4 e-paper reader (ESP32-S3R8)";
}

static const TypeInfo murphy_m4_info = {
    .name = TYPE_MURPHY_M4_MACHINE,
    .parent = TYPE_ESP32S3_MACHINE,
    .class_init = murphy_m4_machine_class_init,
};

static void esp32s3_machine_type_init(void)
{
    type_register_static(&esp32s3_info);
    type_register_static(&murphy_m4_info);
}

type_init(esp32s3_machine_type_init);
'''
    text = replace_once(text, old_tail, new_tail, "register Murphy machine subtype")
    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

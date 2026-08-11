#!/usr/bin/env python3
"""Apply the functional digital GPIO foundation to pinned Espressif QEMU."""
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

    header = root / "include/hw/gpio/esp32_gpio.h"
    h = header.read_text(encoding="utf-8")
    h = replace_once(h, "#define ESP32_GPIO_CLASS(klass)     OBJECT_CLASS_CHECK(Esp32GpioClass, klass, TYPE_ESP32_GPIO)\n\nREG32(GPIO_STRAP, 0x0038)\n", "#define ESP32_GPIO_CLASS(klass)     OBJECT_CLASS_CHECK(Esp32GpioClass, klass, TYPE_ESP32_GPIO)\n\n#define ESP32_GPIO_PIN_COUNT 64\n#define ESP32_GPIO_PIN_CONFIG_COUNT 64\n\nREG32(GPIO_STRAP, 0x0038)\n", "add GPIO pin-count contract")
    h = replace_once(h, "    MemoryRegion iomem;\n    qemu_irq irq;\n    uint32_t strap_mode;\n", "    MemoryRegion iomem;\n    qemu_irq irq;\n    qemu_irq gpio_out[ESP32_GPIO_PIN_COUNT];\n    uint32_t strap_mode;\n    uint64_t input_default;\n    uint64_t input;\n    uint64_t output;\n    uint64_t enable;\n    uint64_t status;\n    uint32_t pin_config[ESP32_GPIO_PIN_CONFIG_COUNT];\n", "add GPIO digital state")
    header.write_text(h, encoding="utf-8")

    source = root / "hw/gpio/esp32_gpio.c"
    c = source.read_text(encoding="utf-8")
    marker = '#include "hw/gpio/esp32_gpio.h"\n\n\n\n'
    helpers = r'''#include "hw/gpio/esp32_gpio.h"

#define GPIO_OUT_OFF              0x004
#define GPIO_OUT_W1TS_OFF         0x008
#define GPIO_OUT_W1TC_OFF         0x00c
#define GPIO_OUT1_OFF             0x010
#define GPIO_OUT1_W1TS_OFF        0x014
#define GPIO_OUT1_W1TC_OFF        0x018
#define GPIO_ENABLE_OFF           0x020
#define GPIO_ENABLE_W1TS_OFF      0x024
#define GPIO_ENABLE_W1TC_OFF      0x028
#define GPIO_ENABLE1_OFF          0x02c
#define GPIO_ENABLE1_W1TS_OFF     0x030
#define GPIO_ENABLE1_W1TC_OFF     0x034
#define GPIO_STRAP_OFF            0x038
#define GPIO_IN_OFF               0x03c
#define GPIO_IN1_OFF              0x040
#define GPIO_STATUS_OFF           0x044
#define GPIO_STATUS_W1TS_OFF      0x048
#define GPIO_STATUS_W1TC_OFF      0x04c
#define GPIO_STATUS1_OFF          0x050
#define GPIO_STATUS1_W1TS_OFF     0x054
#define GPIO_STATUS1_W1TC_OFF     0x058
#define GPIO_PIN0_OFF             0x074
#define GPIO_PIN_INT_TYPE_SHIFT   7
#define GPIO_PIN_INT_TYPE_MASK    0x7
#define GPIO_PIN_INT_ENA_SHIFT    13
#define GPIO_PIN_INT_ENA_MASK     0x1f

enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE = 1, GPIO_INTR_NEGEDGE = 2,
       GPIO_INTR_ANYEDGE = 3, GPIO_INTR_LOW_LEVEL = 4, GPIO_INTR_HIGH_LEVEL = 5 };

static uint32_t gpio_low(uint64_t value) { return (uint32_t)value; }
static uint32_t gpio_high(uint64_t value) { return (uint32_t)(value >> 32); }
static uint64_t gpio_replace_low(uint64_t former, uint32_t value) { return (former & 0xffffffff00000000ULL) | value; }
static uint64_t gpio_replace_high(uint64_t former, uint32_t value) { return (former & 0x00000000ffffffffULL) | ((uint64_t)value << 32); }
static uint64_t esp32_gpio_pad_state(Esp32GpioState *s) { return (s->input & ~s->enable) | (s->output & s->enable); }
static void esp32_gpio_update_irq(Esp32GpioState *s) { qemu_set_irq(s->irq, s->status != 0); }

static void esp32_gpio_update_outputs(Esp32GpioState *s, uint64_t former_output)
{
    const uint64_t driven = s->output & s->enable;
    uint64_t changed = former_output ^ driven;
    for (int pin = 0; pin < ESP32_GPIO_PIN_COUNT; ++pin) {
        const uint64_t bit = 1ULL << pin;
        if (changed & bit) qemu_set_irq(s->gpio_out[pin], (driven & bit) != 0);
    }
}

static bool esp32_gpio_interrupt_triggered(uint32_t type, bool old_level, bool new_level)
{
    switch (type) {
    case GPIO_INTR_POSEDGE: return !old_level && new_level;
    case GPIO_INTR_NEGEDGE: return old_level && !new_level;
    case GPIO_INTR_ANYEDGE: return old_level != new_level;
    case GPIO_INTR_LOW_LEVEL: return !new_level;
    case GPIO_INTR_HIGH_LEVEL: return new_level;
    default: return false;
    }
}

static void esp32_gpio_set_input(void *opaque, int pin, int level)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    if (pin < 0 || pin >= ESP32_GPIO_PIN_COUNT) return;
    const uint64_t bit = 1ULL << pin;
    const bool old_level = (s->input & bit) != 0;
    const bool new_level = level != 0;
    if (new_level) s->input |= bit; else s->input &= ~bit;
    const uint32_t cfg = s->pin_config[pin];
    const uint32_t int_ena = (cfg >> GPIO_PIN_INT_ENA_SHIFT) & GPIO_PIN_INT_ENA_MASK;
    const uint32_t int_type = (cfg >> GPIO_PIN_INT_TYPE_SHIFT) & GPIO_PIN_INT_TYPE_MASK;
    if (int_ena && esp32_gpio_interrupt_triggered(int_type, old_level, new_level)) {
        s->status |= bit;
        esp32_gpio_update_irq(s);
    }
}

'''
    c = replace_once(c, marker, helpers, "insert GPIO register helpers")
    old_read = r'''static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint64_t r = 0;
    switch (addr) {
    case A_GPIO_STRAP:
        r = s->strap_mode;
        break;

    default:
        break;
    }
    return r;
}
'''
    new_read = r'''static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    const uint64_t pads = esp32_gpio_pad_state(s);
    (void)size;
    if (addr >= GPIO_PIN0_OFF && addr < GPIO_PIN0_OFF + ESP32_GPIO_PIN_CONFIG_COUNT * sizeof(uint32_t) && !(addr & 3))
        return s->pin_config[(addr - GPIO_PIN0_OFF) / sizeof(uint32_t)];
    switch (addr) {
    case GPIO_OUT_OFF: return gpio_low(s->output);
    case GPIO_OUT1_OFF: return gpio_high(s->output);
    case GPIO_ENABLE_OFF: return gpio_low(s->enable);
    case GPIO_ENABLE1_OFF: return gpio_high(s->enable);
    case GPIO_STRAP_OFF: return s->strap_mode;
    case GPIO_IN_OFF: return gpio_low(pads);
    case GPIO_IN1_OFF: return gpio_high(pads);
    case GPIO_STATUS_OFF: return gpio_low(s->status);
    case GPIO_STATUS1_OFF: return gpio_high(s->status);
    default: return 0;
    }
}
'''
    c = replace_once(c, old_read, new_read, "replace GPIO read handler")
    old_write = r'''static void esp32_gpio_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
}
'''
    new_write = r'''static void esp32_gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    const uint32_t v = (uint32_t)value;
    const uint64_t former_output = s->output & s->enable;
    (void)size;
    if (addr >= GPIO_PIN0_OFF && addr < GPIO_PIN0_OFF + ESP32_GPIO_PIN_CONFIG_COUNT * sizeof(uint32_t) && !(addr & 3)) {
        s->pin_config[(addr - GPIO_PIN0_OFF) / sizeof(uint32_t)] = v; return;
    }
    switch (addr) {
    case GPIO_OUT_OFF: s->output = gpio_replace_low(s->output, v); break;
    case GPIO_OUT_W1TS_OFF: s->output |= (uint64_t)v; break;
    case GPIO_OUT_W1TC_OFF: s->output &= ~(uint64_t)v; break;
    case GPIO_OUT1_OFF: s->output = gpio_replace_high(s->output, v); break;
    case GPIO_OUT1_W1TS_OFF: s->output |= (uint64_t)v << 32; break;
    case GPIO_OUT1_W1TC_OFF: s->output &= ~((uint64_t)v << 32); break;
    case GPIO_ENABLE_OFF: s->enable = gpio_replace_low(s->enable, v); break;
    case GPIO_ENABLE_W1TS_OFF: s->enable |= (uint64_t)v; break;
    case GPIO_ENABLE_W1TC_OFF: s->enable &= ~(uint64_t)v; break;
    case GPIO_ENABLE1_OFF: s->enable = gpio_replace_high(s->enable, v); break;
    case GPIO_ENABLE1_W1TS_OFF: s->enable |= (uint64_t)v << 32; break;
    case GPIO_ENABLE1_W1TC_OFF: s->enable &= ~((uint64_t)v << 32); break;
    case GPIO_STATUS_OFF: s->status = gpio_replace_low(s->status, v); esp32_gpio_update_irq(s); return;
    case GPIO_STATUS_W1TS_OFF: s->status |= (uint64_t)v; esp32_gpio_update_irq(s); return;
    case GPIO_STATUS_W1TC_OFF: s->status &= ~(uint64_t)v; esp32_gpio_update_irq(s); return;
    case GPIO_STATUS1_OFF: s->status = gpio_replace_high(s->status, v); esp32_gpio_update_irq(s); return;
    case GPIO_STATUS1_W1TS_OFF: s->status |= (uint64_t)v << 32; esp32_gpio_update_irq(s); return;
    case GPIO_STATUS1_W1TC_OFF: s->status &= ~((uint64_t)v << 32); esp32_gpio_update_irq(s); return;
    default: return;
    }
    esp32_gpio_update_outputs(s, former_output);
}
'''
    c = replace_once(c, old_write, new_write, "replace GPIO write handler")
    c = replace_once(c, "static void esp32_gpio_reset_hold(Object *obj, ResetType type)\n{\n}\n", "static void esp32_gpio_reset_hold(Object *obj, ResetType type)\n{\n    Esp32GpioState *s = ESP32_GPIO(obj);\n    (void)type;\n    s->input = s->input_default;\n    s->output = 0; s->enable = 0; s->status = 0;\n    memset(s->pin_config, 0, sizeof(s->pin_config));\n    for (int pin = 0; pin < ESP32_GPIO_PIN_COUNT; ++pin) qemu_set_irq(s->gpio_out[pin], 0);\n    esp32_gpio_update_irq(s);\n}\n", "implement GPIO reset")
    c = replace_once(c, "    sysbus_init_mmio(sbd, &s->iomem);\n    sysbus_init_irq(sbd, &s->irq);\n}\n", "    sysbus_init_mmio(sbd, &s->iomem);\n    sysbus_init_irq(sbd, &s->irq);\n    qdev_init_gpio_in_named(DEVICE(s), esp32_gpio_set_input, \"gpio-in\", ESP32_GPIO_PIN_COUNT);\n    qdev_init_gpio_out_named(DEVICE(s), s->gpio_out, \"gpio-out\", ESP32_GPIO_PIN_COUNT);\n}\n", "expose QEMU GPIO lines")
    c = replace_once(c, "    DEFINE_PROP_UINT32(\"strap_mode\", Esp32GpioState, strap_mode, 0),\n    DEFINE_PROP_END_OF_LIST(),\n", "    DEFINE_PROP_UINT32(\"strap_mode\", Esp32GpioState, strap_mode, 0),\n    DEFINE_PROP_UINT64(\"input-default\", Esp32GpioState, input_default, 0),\n    DEFINE_PROP_END_OF_LIST(),\n", "add idle input property")
    source.write_text(c, encoding="utf-8")

    s3 = root / "hw/xtensa/esp32s3.c"
    s = s3.read_text(encoding="utf-8")
    s = replace_once(s, "    /* GPIO realization */\n    {\n        sysbus_realize(SYS_BUS_DEVICE(&ss->gpio), &error_fatal);\n        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->gpio), 0);\n        memory_region_add_subregion_overlap(sys_mem, DR_REG_GPIO_BASE, mr, 0);\n    }\n", "    /* GPIO realization */\n    {\n        sysbus_realize(SYS_BUS_DEVICE(&ss->gpio), &error_fatal);\n        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->gpio), 0);\n        memory_region_add_subregion_overlap(sys_mem, DR_REG_GPIO_BASE, mr, 0);\n        sysbus_connect_irq(SYS_BUS_DEVICE(&ss->gpio), 0, qdev_get_gpio_in(intmatrix_dev, ETS_GPIO_INTR_SOURCE));\n    }\n", "connect S3 GPIO IRQ")
    s3.write_text(s, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

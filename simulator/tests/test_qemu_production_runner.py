from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


QEMU_DIR = Path(__file__).resolve().parents[1] / "qemu"
MODULE_PATH = QEMU_DIR / "run_production_bin.py"
if str(QEMU_DIR) not in sys.path:
    sys.path.insert(0, str(QEMU_DIR))
SPEC = importlib.util.spec_from_file_location("run_production_bin", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
run_production_bin = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_production_bin)


class ProductionRunnerCommandTests(unittest.TestCase):
    def build(self, **overrides):
        args = dict(
            psram_mb=8,
            gpio_input_default=run_production_bin.MURPHY_IDLE_GPIO_INPUTS,
            efuse_file=None,
            sd_image=None,
            sd_read_only=False,
            serial_file=None,
            gdb=False,
            disable_wdt=False,
            open_eth=False,
            extra=[],
        )
        args.update(overrides)
        return run_production_bin.build_cmd(
            "/opt/qemu-system-xtensa", Path("/tmp/factory.bin"), **args
        )

    def test_production_contract_uses_murphy_machine_and_octal_psram(self):
        joined = " ".join(self.build())
        self.assertIn("-machine murphy-m4", joined)
        self.assertIn("file=/tmp/factory.bin,if=mtd,format=raw", joined)
        self.assertIn("driver=ssi_psram,property=is_octal,value=true", joined)
        self.assertNotIn("wdt_disable", joined)
        self.assertNotIn("open_eth", joined)
        self.assertNotIn("M4_QEMU_BUILD", joined)

    def test_generic_s3_diagnostic_machine_remains_selectable(self):
        self.assertIn("-machine esp32s3", " ".join(self.build(machine="esp32s3")))

    def test_touch_fixture_sets_ft6x36_properties(self):
        joined = " ".join(self.build(touch=(123, 456)))
        self.assertIn("driver=murphy-ft6x36,property=pressed,value=on", joined)
        self.assertIn("driver=murphy-ft6x36,property=x,value=123", joined)
        self.assertIn("driver=murphy-ft6x36,property=y,value=456", joined)

    def test_touch_fixture_rejected_on_generic_machine(self):
        with self.assertRaises(run_production_bin.RunnerError):
            self.build(machine="esp32s3", touch=(1, 1))

    def test_touch_parser_enforces_panel_geometry(self):
        self.assertEqual(run_production_bin.parse_touch("479,799"), (479, 799))
        with self.assertRaises(argparse.ArgumentTypeError):
            run_production_bin.parse_touch("480,10")
        with self.assertRaises(argparse.ArgumentTypeError):
            run_production_bin.parse_touch("bad")

    def test_murphy_idle_gpio_mask_keeps_active_low_keys_released(self):
        mask = run_production_bin.MURPHY_IDLE_GPIO_INPUTS
        self.assertEqual(mask & 0x7, 0x7)
        self.assertTrue(mask & (1 << 44))
        self.assertIn(
            "driver=esp32s3.gpio,property=input-default,value=0x100000000007",
            " ".join(self.build()),
        )

    def test_gpio_mask_must_fit_u64(self):
        with self.assertRaises(run_production_bin.RunnerError):
            self.build(gpio_input_default=1 << 64)

    def test_efuse_backing_uses_esp32s3_nvram_device(self):
        cmd = self.build(
            efuse_file=Path("/tmp/efuse.bin"),
            serial_file=Path("/tmp/serial.log"),
            gdb=True,
            disable_wdt=True,
            open_eth=True,
            extra=["-d", "guest_errors"],
        )
        joined = " ".join(cmd)
        self.assertIn("file=/tmp/efuse.bin,if=none,format=raw,id=efuse", joined)
        self.assertIn("driver=nvram.esp32s3.efuse,property=drive,value=efuse", joined)
        self.assertIn("driver=timer.esp32c3.timg,property=wdt_disable,value=true", joined)
        self.assertIn("-nic user,model=open_eth", joined)
        self.assertIn("-gdb tcp::3333 -S", joined)
        self.assertTrue(cmd[-2:] == ["-d", "guest_errors"])

    def test_sd_image_uses_native_if_sd_controller_path(self):
        joined = " ".join(self.build(sd_image=Path("/tmp/card.img")))
        self.assertIn("file=/tmp/card.img,if=sd,format=raw", joined)
        self.assertNotIn("readonly=on", joined)

    def test_sd_image_can_be_read_only(self):
        self.assertIn(
            "file=/tmp/card.img,if=sd,format=raw,readonly=on",
            " ".join(self.build(sd_image=Path("/tmp/card.img"), sd_read_only=True)),
        )

    def test_sd_image_requires_sector_multiple(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "bad.img"
            path.write_bytes(b"x" * 513)
            with self.assertRaises(run_production_bin.RunnerError):
                run_production_bin.validate_sd_image(path)

    def test_existing_file_rejects_empty_input(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "empty.bin"
            path.write_bytes(b"")
            with self.assertRaises(run_production_bin.RunnerError):
                run_production_bin.existing_file(str(path), "test")


if __name__ == "__main__":
    unittest.main()

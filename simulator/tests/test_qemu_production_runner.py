from __future__ import annotations

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

    def test_production_contract_enables_octal_psram_without_guest_shim(self):
        cmd = self.build()
        joined = " ".join(cmd)
        self.assertIn("-machine esp32s3", joined)
        self.assertIn("file=/tmp/factory.bin,if=mtd,format=raw", joined)
        self.assertIn("driver=ssi_psram,property=is_octal,value=true", joined)
        self.assertNotIn("wdt_disable", joined)
        self.assertNotIn("open_eth", joined)
        self.assertNotIn("M4_QEMU_BUILD", joined)

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
        cmd = self.build(sd_image=Path("/tmp/card.img"))
        joined = " ".join(cmd)
        self.assertIn("file=/tmp/card.img,if=sd,format=raw", joined)
        self.assertNotIn("readonly=on", joined)

    def test_sd_image_can_be_read_only(self):
        cmd = self.build(sd_image=Path("/tmp/card.img"), sd_read_only=True)
        self.assertIn("file=/tmp/card.img,if=sd,format=raw,readonly=on", " ".join(cmd))

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

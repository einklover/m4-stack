import tempfile
from pathlib import Path
import unittest

from tools import murphy_flash_image as mfi


class MurphyFlashImageTests(unittest.TestCase):
    def write_file(self, root: Path, name: str, data: bytes) -> Path:
        path = root / name
        path.write_bytes(data)
        return path

    def test_blank_image_auto_mirrors_application(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bootloader = self.write_file(root, "bootloader.bin", b"B" * 64)
            partitions = self.write_file(root, "partitions.bin", b"P" * 64)
            firmware = self.write_file(root, "firmware.bin", b"APP" * 257)
            output = root / "flash.bin"

            manifest = mfi.compose_image(
                output=output,
                bootloader=bootloader,
                partitions=partitions,
                firmware=firmware,
                slot_mode="auto",
            )

            image = output.read_bytes()
            app = firmware.read_bytes()
            self.assertEqual(len(image), mfi.FLASH_SIZE)
            self.assertEqual(manifest["slot_mode_effective"], "mirror")
            self.assertEqual(image[mfi.APP0_OFFSET : mfi.APP0_OFFSET + len(app)], app)
            self.assertEqual(image[mfi.APP1_OFFSET : mfi.APP1_OFFSET + len(app)], app)
            self.assertEqual(image[0x5000], 0xFF)
            self.assertTrue(output.with_suffix(".bin.json").is_file())

    def test_base_flash_auto_preserves_factory_and_overlays_app1_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "factory.bin"
            base.write_bytes(b"\xA5" * mfi.FLASH_SIZE)
            firmware = self.write_file(root, "firmware.bin", b"NEWAPP")
            output = root / "flash.bin"

            manifest = mfi.compose_image(
                output=output,
                firmware=firmware,
                base_flash=base,
                slot_mode="auto",
            )

            image = output.read_bytes()
            self.assertEqual(manifest["slot_mode_effective"], "app1")
            self.assertEqual(image[mfi.APP0_OFFSET : mfi.APP0_OFFSET + 6], b"\xA5" * 6)
            self.assertEqual(image[mfi.APP1_OFFSET : mfi.APP1_OFFSET + 6], b"NEWAPP")
            self.assertEqual(image[mfi.OTA_DATA_OFFSET], 0xA5)

    def test_firmware_must_fit_murphy_slot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bootloader = self.write_file(root, "bootloader.bin", b"B")
            partitions = self.write_file(root, "partitions.bin", b"P")
            firmware = root / "firmware.bin"
            with firmware.open("wb") as handle:
                handle.seek(mfi.APP_SLOT_SIZE)
                handle.write(b"X")

            with self.assertRaises(mfi.ImageError):
                mfi.compose_image(
                    output=root / "flash.bin",
                    bootloader=bootloader,
                    partitions=partitions,
                    firmware=firmware,
                )

    def test_named_segments_cannot_overlap(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bootloader = self.write_file(root, "bootloader.bin", b"B" * 0x100)
            partitions = self.write_file(root, "partitions.bin", b"P" * 0x100)
            firmware = self.write_file(root, "firmware.bin", b"F" * 0x100)
            extra = self.write_file(root, "extra.bin", b"X" * 0x80)

            with self.assertRaises(mfi.ImageError):
                mfi.compose_image(
                    output=root / "flash.bin",
                    bootloader=bootloader,
                    partitions=partitions,
                    firmware=firmware,
                    extras=[(mfi.APP0_OFFSET + 0x20, extra)],
                )


if __name__ == "__main__":
    unittest.main()

import tempfile
from pathlib import Path
import unittest

from qemu import run_full_flash as runner


class QemuFullFlashRunnerTests(unittest.TestCase):
    def test_requires_exact_16_mib_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.bin"
            path.write_bytes(b"x" * 128)
            with self.assertRaises(runner.RunnerError):
                runner.validate_flash(path)

    def test_rejects_stale_project_flash_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            qemu_dir = Path(tmp)
            (qemu_dir / "sdkconfig").write_text(
                "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y\n", encoding="utf-8"
            )
            with self.assertRaises(runner.RunnerError):
                runner.validate_existing_sdkconfig(qemu_dir)

            (qemu_dir / "sdkconfig").write_text(
                "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y\n", encoding="utf-8"
            )
            runner.validate_existing_sdkconfig(qemu_dir)

    def test_command_uses_external_flash_file(self):
        flash = Path("/tmp/murphy.bin")
        commands = runner.command_lines(
            idf_py="idf.py",
            build_dir="build-fullflash",
            flash_image=flash,
            gdb=True,
            graphics=True,
            no_build=False,
        )
        self.assertEqual(commands[0], ["idf.py", "-B", "build-fullflash", "build"])
        self.assertEqual(
            commands[1],
            [
                "idf.py",
                "-B",
                "build-fullflash",
                "qemu",
                "--flash-file",
                str(flash),
                "--gdb",
                "--graphics",
                "monitor",
            ],
        )


if __name__ == "__main__":
    unittest.main()

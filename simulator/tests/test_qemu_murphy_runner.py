import tempfile
from pathlib import Path
import unittest

from qemu import run_murphy_bin as runner


class QemuMurphyRunnerTests(unittest.TestCase):
    def test_extracts_last_complete_frame_and_writes_rotated_pbm(self):
        first = "FF" * 8
        second = "FF" * 7 + "FE"  # black physical pixel (7, 7)
        log = (
            f"[M4-QEMU-FB] BEGIN 8 8 8\n[M4-QEMU-FB] D {first}\n"
            "[M4-QEMU-FB] END\n"
            f"[M4-QEMU-FB] BEGIN 8 8 8\n[M4-QEMU-FB] D {second}\n"
            "[M4-QEMU-FB] END\n"
        )
        width, height, frame = runner.extract_last_frame(log)
        self.assertEqual((width, height, frame), (8, 8, bytes.fromhex(second)))

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "screen.pbm"
            runner.write_portrait_pbm(output, width, height, frame)
            self.assertEqual(output.read_bytes(), b"P4\n8 8\n\x00\x00\x00\x00\x00\x00\x00\x80")

    def test_rejects_incomplete_frame(self):
        with self.assertRaises(runner.RunnerError):
            runner.extract_last_frame("[M4-QEMU-FB] BEGIN 800 480 48000\n")


if __name__ == "__main__":
    unittest.main()

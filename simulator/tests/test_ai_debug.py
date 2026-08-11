import argparse
import json
import tempfile
from pathlib import Path
import unittest

from tools import ai_debug


class AiDebugTests(unittest.TestCase):
    def test_levels_are_exact_or_cumulative_and_never_touch_hardware(self):
        args = argparse.Namespace(
            level=4,
            through=False,
            output_dir="build/ai-debug-test",
            firmware_dir="../wap-checkpoint/firmware",
            seeds="1:10",
            scenario=None,
            qemu_seconds=3.0,
        )
        steps, _ = ai_debug.build_steps(args)
        self.assertEqual(
            [step["name"] for step in steps],
            ["firmware-build", "flash-compose", "qemu-screen"],
        )
        command_text = " ".join(word for step in steps for word in step["command"])
        self.assertNotIn("m4adb", command_text)
        self.assertNotIn("flash_app1", command_text)
        self.assertEqual(ai_debug.selected_levels(2, False), [2])
        self.assertEqual(ai_debug.selected_levels(2, True), [0, 1, 2])

    def test_qemu_acceptance_requires_a_nonblank_portrait_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe = root / "probe.json"
            screen = root / "screen.pbm"
            probe.write_text(
                json.dumps({"failure_class": None, "acceptance": {"firmware_setup_started": True}}),
                encoding="utf-8",
            )
            screen.write_bytes(b"P4\n480 800\n" + b"\0" * 47999 + b"\x80")
            summary = {}
            error = ai_debug._validate_qemu(summary, {"probe": probe, "screen": screen}, True)
            self.assertIsNone(error)
            self.assertEqual(summary["screen"]["black_pixels"], 1)


if __name__ == "__main__":
    unittest.main()

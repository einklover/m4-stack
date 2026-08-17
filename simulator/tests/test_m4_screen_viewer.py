import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    "m4_screen_viewer", Path(__file__).parents[1] / "tools" / "m4_screen_viewer.py"
)
viewer = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(viewer)


class M4ScreenViewerTests(unittest.TestCase):
    def test_rotates_physical_epd_frame_to_portrait(self):
        physical = bytearray(800 * 480 // 8)
        physical[7 * 100] = 0x01  # physical black pixel x=7, y=7
        w, h, logical = viewer.physical_to_logical(800, 480, bytes(physical))
        self.assertEqual((w, h), (480, 800))
        self.assertEqual(logical[7 * 60 + 59], 0x80)  # logical x=472, y=7
        self.assertEqual(sum(logical), 0x80)

    def test_builds_compact_grayscale_photo_data(self):
        raw = bytes([0x80])
        pgm = viewer.pbm_to_pgm_scaled(8, 1, raw, 8, 1)
        self.assertEqual(pgm, b"P5\n8 1\n255\n" + b"\x00" + b"\xff" * 7)


if __name__ == "__main__":
    unittest.main()

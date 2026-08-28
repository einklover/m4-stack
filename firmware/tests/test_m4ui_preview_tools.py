#!/usr/bin/env python3
"""Host contracts for the portable M4 UI preview tools."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "firmware" / "tools"
sys.path.insert(0, str(TOOLS))

import m4ui_preview  # noqa: E402


class M4UiPreviewContracts(unittest.TestCase):
    def test_cjk_fonts_precede_generic_latin_fallbacks(self) -> None:
        def before(candidates: list[str], preferred: tuple[str, ...], fallback: str) -> None:
            fallback_index = candidates.index(fallback)
            for candidate in preferred:
                self.assertLess(candidates.index(candidate), fallback_index, candidate)

        before(
            m4ui_preview.FONT_CANDIDATES,
            (
                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/truetype/arphic/uming.ttc",
            ),
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        )
        before(
            m4ui_preview.FONT_CANDIDATES,
            (
                "/System/Library/Fonts/PingFang.ttc",
                "/System/Library/Fonts/Hiragino Sans GB.ttc",
                "/System/Library/Fonts/STHeiti Light.ttc",
                "/System/Library/Fonts/Supplemental/Songti.ttc",
            ),
            "/Library/Fonts/Arial.ttf",
        )
        before(
            m4ui_preview.FONT_CANDIDATES,
            ("C:/Windows/Fonts/msyh.ttc",),
            "C:/Windows/Fonts/arial.ttf",
        )
        before(
            m4ui_preview.BOLD_CANDIDATES,
            (
                "/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc",
                "/usr/share/fonts/truetype/arphic/uming.ttc",
            ),
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        )
        before(
            m4ui_preview.BOLD_CANDIDATES,
            (
                "/System/Library/Fonts/PingFang.ttc",
                "/System/Library/Fonts/Hiragino Sans GB.ttc",
                "/System/Library/Fonts/STHeiti Medium.ttc",
            ),
            "/Library/Fonts/Arial Bold.ttf",
        )
        before(
            m4ui_preview.BOLD_CANDIDATES,
            ("C:/Windows/Fonts/msyhbd.ttc",),
            "C:/Windows/Fonts/arialbd.ttf",
        )

    def test_help_does_not_require_pillow_or_a_font(self) -> None:
        result = subprocess.run(
            [sys.executable, "-S", str(TOOLS / "m4ui_preview.py"), "--help"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--font", result.stdout)
        self.assertIn("--gallery", result.stdout)

    def test_explicit_font_override_is_used_and_missing_font_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            font = Path(temporary) / "chosen.ttf"
            font.touch()
            regular, bold = m4ui_preview.resolve_font_paths(font=font)
            self.assertEqual(regular, font.resolve())
            self.assertEqual(bold, font.resolve())
            with self.assertRaises(ValueError):
                m4ui_preview.resolve_font_paths(font=Path(temporary) / "missing.ttf")

    def test_default_output_is_dedicated_and_source_paths_are_rejected(self) -> None:
        args = m4ui_preview.build_parser().parse_args([])
        self.assertEqual(args.out.resolve(), (TOOLS / "preview_output").resolve())
        self.assertNotIn("firmware/src", str(args.out.resolve()))
        with self.assertRaises(ValueError):
            m4ui_preview.validate_output_dir(ROOT / "firmware" / "src" / "generated")

    def test_single_and_batch_pngs_are_480x800_and_gallery_is_bounded(self) -> None:
        font = next(
            (Path(candidate) for candidate in m4ui_preview.FONT_CANDIDATES if Path(candidate).exists()),
            None,
        )
        if font is None:
            self.skipTest("no system font available for raster preview")
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            single = m4ui_preview.render(output, "home", font=font)
            batch = m4ui_preview.render(output, "both", font=font)
            gallery = m4ui_preview.write_gallery(output)
            self.assertEqual([path.name for path in single], ["home_fixed_ui.png"])
            self.assertEqual({path.name for path in batch}, {"home_fixed_ui.png", "detail_fixed_ui.png"})
            from PIL import Image

            for path in batch:
                with Image.open(path) as image:
                    self.assertEqual(image.size, (480, 800))
            self.assertEqual(gallery, output.resolve() / "index.html")
            self.assertIn("home_fixed_ui.png", gallery.read_text(encoding="utf-8"))

    def test_pixel_html_uses_the_same_480x800_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "m4ui_pixel_html.py"),
                    "--screen",
                    "home",
                    "--out",
                    temporary,
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            html = Path(temporary) / "home.html"
            self.assertTrue(html.exists())
            contents = html.read_text(encoding="utf-8")
            self.assertIn("width:480px", contents)
            self.assertIn("height:800px", contents)


if __name__ == "__main__":
    unittest.main()

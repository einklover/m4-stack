#!/usr/bin/env python3
"""Static contract for the minimalist M4 default shutdown wallpaper."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
WALLPAPER = ROOT / "firmware" / "src" / "util" / "CrosslinkDefaultWallpaper.h"
SLEEP = ROOT / "firmware" / "src" / "activities" / "boot_sleep" / "SleepActivity.cpp"


def test_m4_default_shutdown_wallpaper_is_minimal_and_light() -> None:
    wallpaper = WALLPAPER.read_text(encoding="utf-8")
    sleep = SLEEP.read_text(encoding="utf-8")

    assert "images/bg.h" not in wallpaper
    assert "renderer.clearScreen();" in wallpaper
    assert wallpaper.count("renderer.drawArc(") == 4
    assert "pageHeight / 2 - 140" in wallpaper
    assert "kCircleRadius = 33" in wallpaper
    assert "circleY + kCircleRadius + 47" in wallpaper
    assert "pageHeight - 82" in wallpaper
    assert 'renderer.drawCenteredText(UI_10_FONT_ID, circleY + kCircleRadius + 47, "休息中")' in wallpaper
    assert 'constexpr float kBrandScale = 0.7f' in wallpaper
    assert 'constexpr char kBrand[] = "CrossPoint"' in wallpaper
    assert 'renderer.getTextWidth(SMALL_FONT_ID, kBrand, EpdFontFamily::REGULAR, kBrandScale)' in wallpaper
    assert 'renderer.drawText(SMALL_FONT_ID, (pageWidth - brandWidth) / 2' in wallpaper
    assert 'EpdFontFamily::REGULAR, kBrandScale' in wallpaper
    assert "drawImage" not in wallpaper

    default = re.search(
        r"void SleepActivity::renderDefaultSleepScreen\(\) const\s*\{(?P<body>.*?)\n\}",
        sleep,
        re.DOTALL,
    )
    assert default, "default sleep renderer is missing"
    body = default.group("body")
    assert "drawCrosslinkDefaultWallpaper(renderer);" in body
    assert "invertScreen" not in body
    assert "displayBuffer(HalDisplay::FAST_REFRESH)" in body


if __name__ == "__main__":
    test_m4_default_shutdown_wallpaper_is_minimal_and_light()
    print("m4 shutdown wallpaper contract: PASS")

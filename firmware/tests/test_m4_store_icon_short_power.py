"""Generated store icon bytes and M4 short-press power decision."""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
PNG = ROOT / "firmware/src/components/icons/source/app_store_generated_96.png"
HEADER = ROOT / "firmware/src/components/icons/app_store_generated.h"
DECODER = ROOT / "firmware/src/activities/home/HomeSceneAssetDecoder.cpp"
MAIN = ROOT / "firmware/src/main.cpp"
DRAWER = ROOT / "firmware/src/activities/apps/AppListActivity.cpp"


def m4_short_power_sleep(now_ms, ignore_until_ms, released, held_ms, pressed):
    """Mirror of the M4 loop gate: release once, not while held, after settle."""
    if pressed and not released:
        return False
    if now_ms < ignore_until_ms:
        return False
    if not released:
        return False
    return 60 <= held_ms < 8000


def test_generated_png_and_bitmap():
    assert PNG.is_file(), "missing app_store_generated_96.png"
    image = Image.open(PNG)
    assert image.size == (96, 96)
    assert image.mode == "1"
    text = HEADER.read_text()
    assert "kAppStoreGeneratedIcon[512]" in text
    assert text.count("0x") == 512
    decoder = DECODER.read_text()
    assert 'strcmp(id, "builtin.store") == 0) return kAppStoreGeneratedIcon' in decoder
    assert "LibraryIcon" not in decoder
    drawer = DRAWER.read_text()
    assert "case UIIcon::Library: return LibraryIcon;" in drawer
    assert "builtinSheetIcon(item.id.c_str())" in drawer
    assert "kAppStoreGeneratedIcon" not in drawer


def test_power_release_state():
    main = MAIN.read_text()
    assert "#ifdef CROSSPOINT_MURPHY_M4" in main
    assert "m4PowerIgnoreUntilMs = millis() + 700" in main
    assert "gpio.wasReleased(HalGPIO::BTN_POWER)" in main
    assert "gpio.getPowerButtonHeldTime()" in main
    assert "getHeldTime()" not in main.split("m4PowerIgnoreUntilMs", 1)[1].split("#else", 1)[0]
    assert "Do not re-sleep when longPressBoot is still set." in main
    assert m4_short_power_sleep(100, 700, True, 120, False) is False
    assert m4_short_power_sleep(800, 700, True, 30, False) is False
    assert m4_short_power_sleep(800, 700, False, 200, True) is False
    assert m4_short_power_sleep(800, 700, True, 180, False) is True
    assert m4_short_power_sleep(800, 700, True, 9000, False) is False


if __name__ == "__main__":
    test_generated_png_and_bitmap()
    test_power_release_state()
    print("ok")

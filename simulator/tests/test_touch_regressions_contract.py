"""Focused contracts for the confirmed M4 touch/lifecycle regressions."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def source(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def function_body(src: str, signature: str) -> str:
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for index in range(brace, len(src)):
        if src[index] == "{":
            depth += 1
        elif src[index] == "}":
            depth -= 1
            if depth == 0:
                return src[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


class TouchRegressionContracts(unittest.TestCase):
    def test_home_and_library_tap_wins_over_same_frame_touch_down(self) -> None:
        home = function_body(source("firmware/src/activities/home/HomeActivity.cpp"), "void HomeActivity::loop()")
        library = function_body(source("firmware/src/activities/home/MyLibraryActivity.cpp"), "void MyLibraryActivity::loop()")
        recent = function_body(source("firmware/src/activities/home/RecentBooksActivity.cpp"), "void RecentBooksActivity::loop()")

        self.assertIn("const bool tapped = mappedInput.wasScreenTapped", home)
        self.assertIn("wasScreenTouchDown(tx, ty) && !tapped", home)
        self.assertLess(home.index("if (tapped)"), home.index("const auto swipe"))
        self.assertIn("wasScreenTapped(tx, ty)", library)
        self.assertIn("wasScreenTouchDown(tx, ty)", library)
        self.assertLess(library.index("wasScreenTapped(tx, ty)"), library.index("wasScreenTouchDown(tx, ty)"))
        self.assertIn("M4ListTouchPolicy::mergeFrame", recent)

    def test_rendered_touch_only_dialogs_have_touch_paths(self) -> None:
        home = function_body(source("firmware/src/activities/home/HomeActivity.cpp"), "void HomeActivity::loop()")
        self.assertIn("cancelRect.contains", home)
        self.assertIn("restartRect.contains", home)

        for name in ("ResetSettingsActivity", "ClearCacheActivity", "OnlineOtaActivity"):
            cpp = source(f"firmware/src/activities/settings/{name}.cpp")
            header = source(f"firmware/src/activities/settings/{name}.h")
            self.assertIn("touchFooterButtonsMask", header)
            self.assertIn("wasReleased(MappedInputManager::Button::Confirm)", cpp)
            self.assertIn("wasReleased(MappedInputManager::Button::Back)", cpp)

    def test_home_menu_and_footer_use_painted_geometry(self) -> None:
        home = source("firmware/src/activities/home/HomeActivity.cpp")
        geometry = source("firmware/src/util/TouchHitGeometry.h")
        footer = source("firmware/src/util/M4FooterTouchPolicy.h")
        self.assertIn("lyraMenuIndexFromPoint", home)
        self.assertIn("pageHeight - menuTop - metrics.buttonHintsHeight", home)
        self.assertIn("constexpr int buttonPositions[] = {38, 154, 268, 384};", footer)
        self.assertIn("LyraMenuLayout", geometry)

    def test_lifecycle_and_reader_touch_contracts(self) -> None:
        firmware = ROOT / "firmware/src/activities"
        offenders = []
        for path in firmware.rglob("*.cpp"):
            if path.name == "ActivityWithSubactivity.cpp":
                continue
            if "subActivity->loop();" in path.read_text(encoding="utf-8"):
                offenders.append(str(path))
        self.assertEqual(offenders, [])

        xtc = source("firmware/src/activities/reader/XtcReaderActivity.cpp")
        self.assertIn("TouchHitGeometry::readerZoneFromPoint", xtc)
        self.assertIn("mappedInput.wasMenuGesture()", xtc)
        self.assertIn("touchPrev", xtc)
        self.assertIn("touchNext", xtc)

        epub_menu = function_body(
            source("firmware/src/activities/reader/EpubReaderMenuActivity.cpp"),
            "void EpubReaderMenuActivity::loop()",
        )
        self.assertIn("hintGutterHeight", epub_menu)
        self.assertIn("headerY >= hintGutterHeight", epub_menu)

    def test_server_does_not_claim_unpainted_bottom_strip(self) -> None:
        for name in ("CrossPointWebServerActivity", "CalibreConnectActivity"):
            self.assertNotIn(
                "getScreenHeight() - 92",
                source(f"firmware/src/activities/network/{name}.cpp"),
            )


if __name__ == "__main__":
    unittest.main()

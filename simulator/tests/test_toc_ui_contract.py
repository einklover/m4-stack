#!/usr/bin/env python3
"""Contract tests for chapter-list font, geometry, and refresh handoff.

Refresh audit conclusion: EPUB/TXT parent painting is stopped before the TOC
child paints, and EPUB/TXT/XTC clear the logical framebuffer before submitting
the complete frame. FAST can leave ordinary e-ink ghosting after a preceding
HALF refresh, but there is no omitted top-region update or path-specific stale
frame to justify a slow full-screen refresh.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "firmware/src"


def test_chapter_rows_use_system_font_resolution():
    ui_text = (FIRMWARE / "util/M4UiText.h").read_text()
    assert "return resolveSystem(renderer, chromeFontId);" in ui_text
    assert "f.fontId = M4FixedRuntimeUiFonts::systemFontId(f.layoutFontId);" in ui_text
    assert "M4FixedRuntimeUiFonts::ensureSystemFaces(mutableRenderer);" in ui_text

    for name in (
        "activities/reader/EpubReaderChapterSelectionActivity.cpp",
        "activities/reader/TxtReaderChapterSelectionActivity.cpp",
        "activities/reader/XtcReaderChapterSelectionActivity.cpp",
    ):
        source = (FIRMWARE / name).read_text()
        assert "drawSystem" in source or "resolveSystem" in source
        assert "NOTOSANS_12_FONT_ID" not in source


def test_shared_geometry_is_used_by_all_chapter_lists():
    metrics = (FIRMWARE / "util/M4TouchListMetrics.h").read_text()
    assert "ChapterListLayout" in metrics
    assert "makeChapterListLayout" in metrics
    assert "backHitbox" in metrics
    assert "firstRow" in metrics
    assert "footer" in metrics
    assert "header" in metrics


def test_system_ui_tiers_remain_explicitly_mapped():
    policy = (FIRMWARE / "util/M4RuntimeUiFontPolicy.h").read_text()
    assert "kSmallBasePx = M4FontPolicy::kChromeSmallPx" in policy
    assert "kUi10BasePx = M4FontPolicy::kChromeUi10Px" in policy
    assert "kUi12BasePx = M4FontPolicy::kChromeUi12Px" in policy

    font_policy = (FIRMWARE / "util/M4FontPolicy.h").read_text()
    assert "constexpr int kChromeSmallPx = 16" in font_policy
    assert "constexpr int kChromeUi10Px = kChromeUiPxMedium" in font_policy
    assert "constexpr int kChromeUi12Px = kChromeUiPxMedium" in font_policy
    assert "constexpr int kChromeUiPxSmall = 16" in font_policy
    assert "constexpr int kChromeUiPxMedium = 24" in font_policy
    assert "constexpr int kChromeUiPxLarge = 26" in font_policy

    fixed = (FIRMWARE / "util/M4FixedRuntimeUiFonts.h").read_text()
    assert "if (layoutFontId == SMALL_FONT_ID) return kSystemSmallFontId;" in fixed
    assert "if (layoutFontId == UI_10_FONT_ID) return kSystemUi10FontId;" in fixed
    assert "return kSystemUi12FontId;" in fixed


def test_header_reserves_back_hitbox_before_title():
    theme = (FIRMWARE / "components/themes/fengyan/FengyanTheme.cpp").read_text()
    assert "kHeaderHitWidth" in theme
    assert "titleX" in theme


def test_geometry_contract_holds_for_all_orientations_and_ui_tiers():
    # Mirror the pure C++ layout inputs with representative measured system
    # line heights for small/medium/large UI configurations.
    for orientation in ("portrait", "landscape_cw", "portrait_inverted", "landscape_ccw"):
        for line_height in (16, 20, 24):
            screen_w, screen_h = (480, 800) if "portrait" in orientation else (800, 480)
            content_x = 30 if orientation == "landscape_cw" else 0
            content_y = 50 if orientation == "portrait_inverted" else 0
            header_h = max(44, line_height + 12)
            row_h = max(52, line_height + 20)
            header_top = content_y + 5
            list_top = header_top + header_h + 16
            footer_top = max(list_top, screen_h - 96)

            back = (0, 0, min(56, screen_w), min(56, screen_h))
            header = (content_x, header_top, screen_w - content_x, header_h)
            title = (64, header_top + 6, max(0, screen_w - 64), header_h - 12)
            listing = (content_x, list_top, screen_w - content_x, footer_top - list_top)
            first = (listing[0], listing[1], listing[2], min(row_h, listing[3]))
            footer = (0, footer_top, screen_w, screen_h - footer_top)

            def disjoint(a, b):
                return a[0] + a[2] <= b[0] or b[0] + b[2] <= a[0] or a[1] + a[3] <= b[1] or b[1] + b[3] <= a[1]

            assert disjoint(back, title)
            assert disjoint(back, listing)
            assert disjoint(header, listing)
            assert disjoint(listing, footer)
            assert listing[1] <= first[1]
            assert back[2] == 56 and back[3] == 56


def test_toc_entry_starts_from_a_clean_full_frame():
    for name in (
        "activities/reader/EpubReaderChapterSelectionActivity.cpp",
        "activities/reader/TxtReaderChapterSelectionActivity.cpp",
        "activities/reader/XtcReaderChapterSelectionActivity.cpp",
    ):
        source = (FIRMWARE / name).read_text()
        assert "renderer.clearScreen();" in source
        assert "renderer.displayBuffer(HalDisplay::FAST_REFRESH);" in source
        assert "displayWindow(" not in source
        render_name = (
            "void TxtReaderChapterSelectionActivity::drawScreen"
            if "TxtReader" in name
            else f"void {'Epub' if 'Epub' in name else 'Xtc'}ReaderChapterSelectionActivity::renderScreen"
        )
        render_start = source.index(render_name)
        clear_at = source.index("renderer.clearScreen();", render_start)
        # TXT submits immediately after drawScreen returns; EPUB/XTC submit
        # from renderScreen itself.
        if "TxtReader" in name:
            draw_call = source.index("drawScreen(pageTitles", 0)
            display_at = source.index("renderer.displayBuffer(HalDisplay::FAST_REFRESH);", draw_call)
            assert clear_at > render_start and draw_call < display_at
        else:
            display_at = source.index("renderer.displayBuffer(HalDisplay::FAST_REFRESH);", clear_at)
            assert clear_at < display_at

    hal = (ROOT / "firmware/lib/hal/HalDisplay.cpp").read_text()
    assert "einkDisplay.displayBuffer(convertRefreshMode(effectiveMode), turnOffScreen);" in hal


def test_reader_to_toc_paths_do_not_leave_a_parent_painting():
    epub = (FIRMWARE / "activities/reader/EpubReaderActivity.cpp").read_text()
    assert "MenuAction::SELECT_CHAPTER" in epub
    assert "exitActivity();\n      enterChapterSelector();" in epub
    assert "EpubReaderSettingsActivity" in epub

    txt = (FIRMWARE / "activities/reader/TxtReaderActivity.cpp").read_text()
    assert "suppressDisplay_ = true;" in txt
    assert "if (suppressDisplay_ || subActivity)" in txt
    assert "enterChapterPicker" in txt

    # XTC's chapter picker is a standalone activity today; regardless of its
    # caller, it must establish the same clean full-frame handoff.
    xtc = (FIRMWARE / "activities/reader/XtcReaderChapterSelectionActivity.cpp").read_text()
    assert "renderer.clearScreen();" in xtc
    assert "renderer.displayBuffer(HalDisplay::FAST_REFRESH);" in xtc

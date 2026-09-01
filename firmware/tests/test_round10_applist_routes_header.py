"""Round 12 host contract: builtin destinations and right-half wifi + aligned status cluster."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
APP_H = ROOT / "firmware/src/activities/apps/AppListActivity.h"
APP_CPP = ROOT / "firmware/src/activities/apps/AppListActivity.cpp"
MAIN_CPP = ROOT / "firmware/src/main.cpp"
FENGYAN_H = ROOT / "firmware/src/components/themes/fengyan/FengyanTheme.h"
FENGYAN_CPP = ROOT / "firmware/src/components/themes/fengyan/FengyanTheme.cpp"
HOME_REF_H = ROOT / "firmware/src/util/HomeRef.h"


def _source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _function(source: str, signature: str) -> str:
    # Declarations appear above the definitions in main.cpp; use the final
    # occurrence so the brace walk starts at the function body.
    start = source.rindex(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_builtin_labels_use_explicit_named_destinations():
    app_h = _source(APP_H)
    app_cpp = _source(APP_CPP)
    main_cpp = _source(MAIN_CPP)

    assert "FileManager," in app_h
    assert "onFileManagerOpen" in app_h
    assert 'addBuiltin(BuiltinAction::FileManager, "builtin.files", L(Str::kFileManager)' in app_cpp
    assert "case BuiltinAction::FileManager:" in app_cpp
    assert "callbacks_.onFileManagerOpen" in app_cpp

    # Every drawer callback is assigned by field name, so adding/reordering a
    # callback cannot silently shift DataCapsule/Network/Settings destinations.
    for field, destination in (
        ("onSettingsOpen", "onGoToSettings"),
        ("onFileManagerOpen", "onGoToMyLibrary"),
        ("onRecentBooksOpen", "onGoToRecentBooks"),
        ("onOpdsOpen", "onGoToBrowser"),
        ("onJianGuoOpen", "onGoToJianGuoYun"),
        ("onDataCapsuleOpen", "onGoToDataCapsule"),
        ("onBookmarkNotesOpen", "onGoToBookmarkNotes"),
        ("onNetworkOpen", "onGoToNetwork"),
    ):
        assert f"callbacks.{field} = {destination};" in main_cpp


def test_files_stay_library_and_network_opens_crosspoint_transfer():
    main_cpp = _source(MAIN_CPP)
    native_route = _function(main_cpp, "void onGoToNativeApp(")
    network_route = _function(main_cpp, "void onGoToNetwork()")
    transfer_route = _function(main_cpp, "void onGoToFileTransfer()")

    assert 'if (appId == "builtin.files")' in native_route
    assert "onGoToMyLibrary();" in native_route
    assert "onGoToFileTransfer();" not in native_route

    assert "onGoToFileTransfer();" in network_route
    assert "new SettingsActivity" not in network_route
    assert "SettingsHubCard::NetworkSync" not in network_route
    assert "SettingsPane::Category" not in network_route
    assert "NetworkModeSelectionActivity" not in network_route
    assert "new WifiSelectionActivity" not in network_route
    assert "new CrossPointWebServerActivity" in transfer_route


def test_transfer_stays_reachable_via_usb_debug_hook():
    main_cpp = _source(MAIN_CPP)
    usb_route = _function(main_cpp, "void onGoToFileTransferUsb()")
    assert "new CrossPointWebServerActivity" in usb_route
    assert "true" in usb_route
    assert 'hooks.openFileTransferUi = []() { onGoToFileTransferUsb(); };' in main_cpp


def test_non_home_header_uses_balanced_geometry():
    home_ref = _source(HOME_REF_H)
    fengyan_h = _source(FENGYAN_H)
    fengyan_cpp = _source(FENGYAN_CPP)

    assert "HeaderSafeTop = 0" in home_ref
    assert "HeaderH = 46" in home_ref
    assert "HeaderTitleBaseline = 38" in home_ref
    assert "HeaderY = 0" in home_ref
    assert ".topPadding = HomeRef::HeaderSafeTop" in fengyan_h
    assert ".headerHeight = HomeRef::HeaderH" in fengyan_h
    assert "HomeHeaderTitleBaseline" in fengyan_cpp
    assert "HeaderTitleBaseline" in fengyan_cpp


def test_wifi_glyph_is_right_half_only():
    fengyan_cpp = _source(FENGYAN_CPP)
    # drawWifiGlyph helper must keep 2x2 dot and only the right upper quadrant
    assert "void drawWifiGlyph" in fengyan_cpp
    # Right half is kept: xDir=1, yDir=-1
    assert "drawArc(r, cx, cy, 1, -1" in fengyan_cpp
    # Left half must be deleted: xDir=-1 must not appear inside drawWifiGlyph
    glyph_section = fengyan_cpp[fengyan_cpp.index("void drawWifiGlyph") : fengyan_cpp.index("void drawWifiGlyph") + 800]
    assert "drawArc(r, cx, cy, -1, -1" not in glyph_section
    assert "drawArc(r, cx, cy, -1" not in glyph_section
    # Must not swap in bitmap WifiIcon/Wifi32 path
    assert "WifiIcon" not in glyph_section
    assert "Wifi32" not in glyph_section


def test_header_status_cluster_is_right_packed():
    home_ref = _source(HOME_REF_H)
    fengyan_cpp = _source(FENGYAN_CPP)
    # Parse X constants
    def _parse_int(name: str) -> int:
        for line in home_ref.splitlines():
            if name in line and "constexpr" in line:
                # e.g. constexpr int16_t HeaderWifiX = 364;
                part = line.split("=")[1]
                num = part.strip().split(";")[0].strip().split()[0]
                return int(num)
        raise AssertionError(f"missing {name}")

    screen_w = _parse_int("ScreenW")
    battery_x = _parse_int("HeaderBatteryX")
    battery_w = _parse_int("HeaderBatteryW")
    wifi_x = _parse_int("HeaderWifiX")
    time_x = _parse_int("HeaderTimeX")
    title_x = _parse_int("HeaderTitleX")
    divider_x = _parse_int("HeaderDividerX")
    # Right inset 18-27px
    right_inset = screen_w - (battery_x + battery_w)
    assert 18 <= right_inset <= 27, f"right inset {right_inset} not in 18-27"
    # Order: clock, wifi, battery (battery far right)
    assert time_x < wifi_x < battery_x
    # Even gaps ~10-14px between successive status elements (allow clock width ~38)
    # wifi to battery gap when percent hidden is larger due to text, so we check statusCenter alignment instead
    gap_time_wifi = wifi_x - (time_x + 38)
    assert 8 <= gap_time_wifi <= 16, f"gap time-wifi {gap_time_wifi} not in 8-16"
    # Wifi to battery text/battery cluster should be compact, not sparse 70px
    gap_wifi_battery = battery_x - (wifi_x + 22)
    # When percent shown, wifi to battery holds text, so hidden gap 45 is expected; we check right-packed wifi
    assert 30 <= gap_wifi_battery <= 70 or 10 <= gap_wifi_battery <= 16
    # Title stays left of divider
    assert title_x > divider_x
    assert divider_x == 47
    assert title_x == 58
    # Title max width must stop before clock
    assert "HeaderTimeX - HomeRef::HeaderTitleX - 8" in fengyan_cpp or "HeaderTimeX - HomeRef::HeaderTitleX" in fengyan_cpp
    # Battery double-offset bug must be fixed: drawBattery must not add fontHeight offset when caller already centered
    assert "batteryYOffset" not in fengyan_cpp
    assert "fontHeight - rect.height" not in fengyan_cpp
    # Status vertical centering must use shared center, not rect.y + (HeaderH - Icon)/2 for all separate
    # At least clock and battery should use statusCenter derived from title
    assert "statusCenter" in fengyan_cpp

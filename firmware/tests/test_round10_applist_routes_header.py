"""Round 11 host contract: builtin destinations and balanced non-Home header."""

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

    assert "HeaderSafeTop = 20" in home_ref
    assert "HeaderH = 46" in home_ref
    assert "HeaderTitleBaseline = 38" in home_ref
    assert ".topPadding = HomeRef::HeaderSafeTop" in fengyan_h
    assert ".headerHeight = HomeRef::HeaderH" in fengyan_h
    assert "HomeHeaderTitleBaseline" in fengyan_cpp
    assert "HeaderTitleBaseline" in fengyan_cpp

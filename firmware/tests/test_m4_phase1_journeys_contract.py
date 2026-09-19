#!/usr/bin/env python3
"""Phase 1 host journeys J0–J3 — source contract.

This is the host/Journey round. It does not claim QEMU pixel GREEN.
simulator/journeys/j1_settings_nav.json remains Hub-era (4 px column) and
must stay that way until the QEMU/pixel stage; do not forge screenshot boxes.

Old n1 Settings Hub pins (n1-settings-s1-hub-logical / n1-settings-s2-row-logical)
were RED on integration 10a844d because SettingsActivity no longer contains
settingsNavMoveHub / settingsNavMoveRow+settingsNavSyncWindow. Those checks
must be migrated to the 8-row root contract (not deleted into a fake green).

Run: python3 firmware/tests/test_m4_phase1_journeys_contract.py
"""

from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
JOURNEY_H = REPO / "firmware" / "tests" / "native_app" / "fixtures" / "m4_phase1_journeys.h"
JOURNEY_CPP = REPO / "firmware" / "tests" / "native_app" / "test_m4_phase1_journeys.cpp"
N1 = REPO / "firmware" / "tests" / "test_phase1_n1_adoption_contract.py"
SETTINGS_CPP = REPO / "firmware" / "src" / "activities" / "settings" / "SettingsActivity.cpp"
CATALOG = REPO / "firmware" / "src" / "activities" / "settings" / "M4SettingsCatalog.h"
MAIN = REPO / "firmware" / "src" / "main.cpp"
XFER = REPO / "firmware" / "src" / "activities" / "network" / "CrossPointWebServerActivity.cpp"
MENU_H = REPO / "firmware" / "src" / "activities" / "reader" / "EpubReaderMenuActivity.h"
READER_SET = REPO / "firmware" / "src" / "activities" / "reader" / "EpubReaderSettingsActivity.cpp"
J1_JSON = REPO / "simulator" / "journeys" / "j1_settings_nav.json"
SETTINGS_LISTS = REPO / "firmware" / "src" / "SettingsLists.h"
I18N = REPO / "firmware" / "src" / "I18n.h"
UI_TYPES = REPO / "firmware" / "src" / "ui" / "scene" / "UiSceneTypes.h"
HOST_DOC = REPO / "docs" / "superpowers" / "journeys" / "2026-09-14-phase1-host-journeys.md"

failures = []


def check(name, cond, detail=""):
    print(("PASS" if cond else "FAIL") + ": " + name + ("" if cond else " -- " + detail))
    if not cond:
        failures.append(name)


def read(path):
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def git_diff_empty(rel):
    out = subprocess.check_output(
        ["git", "diff", "f1aed53743fa27c8cd1dabf27f4c7d2678630881", "--", rel],
        cwd=str(REPO),
        text=True,
    )
    return out.strip() == ""


def main():
    check("j-header-exists", JOURNEY_H.is_file(),
          "fixtures/m4_phase1_journeys.h missing (J0-J3 walk oracles)")
    check("j-native-exists", JOURNEY_CPP.is_file(),
          "test_m4_phase1_journeys.cpp missing")
    check("j-host-doc-exists", HOST_DOC.is_file(),
          "host journey ledger doc missing")

    header = read(JOURNEY_H)
    native = read(JOURNEY_CPP)
    n1 = read(N1)
    settings = read(SETTINGS_CPP)
    catalog = read(CATALOG)
    main_cpp = read(MAIN)
    xfer = read(XFER)
    menu = read(MENU_H)
    reader = read(READER_SET)
    j1 = read(J1_JSON)
    types = read(UI_TYPES)

    for token in ("kM4JourneyJ0OldRed", "kM4JourneyJ1OldRed", "kM4JourneyJ2OldRed",
                  "kM4JourneyJ3OldRed", "m4JourneyEnterRoot", "m4JourneySettingsBackFromChild",
                  "m4JourneyWifiPopReset", "m4JourneySimulateExternalPop"):
        check("j-header-" + token, token in header, "journey header missing " + token)

    for token in ("testJ0EightRowIdentityAndHubRetired", "testJ1SettingsProductWalks",
                  "testJ2WifiTransferWalks", "testJ3ReaderWalks", "testCrossModuleStability"):
        check("j-native-" + token, token in native, "native journey test missing " + token)

    # --- J0: n1 Settings Hub pins must be migrated, not left RED or deleted ---
    check("j0-n1-hub-pin-retired",
          "n1-settings-s1-hub-logical" not in n1 and '"settingsNavMoveHub" in settings' not in n1,
          "n1 still requires Hub-era settingsNavMoveHub (old RED on 10a844d)")
    check("j0-n1-row-pin-retired",
          "n1-settings-s2-row-logical" not in n1 and '"settingsNavMoveRow" in settings' not in n1,
          "n1 still requires Hub-era settingsNavMoveRow+SyncWindow (old RED on 10a844d)")
    check("j0-n1-hub-tokens-gone-from-activity",
          "settingsNavMoveHub" not in settings and "settingsNavMoveRow" not in settings
          and "settingsNavSyncWindow" not in settings,
          "SettingsActivity reintroduced Hub nav tokens")
    check("j0-n1-migrated-clamp",
          "m4SettingsUiMove" in n1 and "m4SettingsWholeRowHit" in n1 and "selectedKey" in n1,
          "n1 Settings section was not migrated to 8-row clamp/whole-row/selectedKey")
    check("j0-n1-root-keys",
          "readerLayout" in n1 and "maintenance" in n1 and "advanced" in n1,
          "n1 must pin the 8 root keys including 阅读/维护/高级")
    check("j0-catalog-eight",
          "kM4SettingsRootCount = 8" in catalog or "kM4SettingsRootCount = 8" in catalog.replace(" ", ""),
          "root catalog is not 8 rows")
    for key in ("wifi", "frontlight", "readerLayout", "sleepTimeout", "sleepScreen",
                "keys", "maintenance", "advanced"):
        check("j0-catalog-" + key, '"' + key + '"' in catalog, "catalog missing " + key)

    # --- J1 Settings journeys in production ---
    check("j1-reader-door", "EpubReaderSettingsActivity" in settings and "readerLayout" in settings)
    check("j1-wifi-door", "WifiSelectionActivity" in settings)
    check("j1-wifi-pop-reset", "m4SettingsPaintResetFull" in settings)
    check("j1-logical-nav", "onPreviousRelease" in settings and "onNextRelease" in settings)
    check("j1-whole-row", "m4SettingsWholeRowHit" in settings)
    check("j1-power-not-confirm", "m4SettingsPowerIsPrimary" in settings or "M4ConfirmButton::Power" in settings)
    check("j1-back-child", "m4SettingsUiEnterRoot" in settings and "parentKey" in settings)

    # --- J2 Wi-Fi vs Transfer ---
    check("j2-network-not-transfer",
          "onGoToFileTransfer();" not in main_cpp.split("void onGoToNetwork()")[1].split("void ")[0]
          if "void onGoToNetwork()" in main_cpp else False,
          "onGoToNetwork still aliases 传书")
    check("j2-system-networking", "SystemNetworking" in main_cpp)
    check("j2-transfer-picker-gate", "m4TransferStartsWithExistingSta" in xfer)
    check("j2-no-s3-claim", "S3 共存" not in (main_cpp + xfer + settings) and "s3 coexist" not in (main_cpp + xfer).lower())

    # --- J3 Reader ---
    check("j3-quick-layout", '"排版"' in menu and "OPEN_STYLE" in menu)
    check("j3-unique-reading-settings", '"阅读设置"' in menu)
    check("j3-all-books", "全部书籍" in reader)
    check("j3-font-picker", "FontSelectionActivity" in reader)
    check("j3-no-per-book", "BookTypographyOverride" not in menu and "per-book" not in menu)

    # --- Cross-module / frozen files ---
    check("j-kmax-8", "kMaxRepeatItems = 8" in types)
    check("j-settingslists-untouched", git_diff_empty("firmware/src/SettingsLists.h"),
          "SettingsLists.h changed vs baseline f1aed53")
    check("j-i18n-untouched", git_diff_empty("firmware/src/I18n.h"),
          "I18n.h changed vs baseline f1aed53")

    # QEMU leftover: must still describe Hub so we do not fake pixel GREEN.
    check("j-qemu-j1-still-hub-era",
          ("hub" in j1.lower() or "card-column" in j1),
          "do not silently rewrite QEMU J1 pixels in this host round")
    check("j-host-not-pixel",
          "screenshot_bbox" not in native and "PBM" not in native,
          "native journeys must not forge pixel screenshots")

    print()
    if failures:
        print("RED: %d failing Phase 1 journey check(s):" % len(failures))
        for name in failures:
            print("  - " + name)
        sys.exit(1)
    print("GREEN: Phase 1 host journeys J0-J3 source contract holds.")


if __name__ == "__main__":
    main()

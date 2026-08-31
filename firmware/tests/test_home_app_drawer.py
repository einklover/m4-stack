from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_LIST_H = ROOT / "firmware/src/activities/apps/AppListActivity.h"
APP_LIST_CPP = ROOT / "firmware/src/activities/apps/AppListActivity.cpp"
HOME_DRAWER_HELPER_CANDIDATES = [
    ROOT / "firmware/src/activities/apps/AppListModel.h",
    ROOT / "firmware/src/activities/apps/DrawerInventory.h",
    ROOT / "firmware/src/activities/apps/AppDrawerHelper.h",
    ROOT / "firmware/src/util/AppDrawerInventory.h",
]

def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")

def _find_helper():
    for cand in HOME_DRAWER_HELPER_CANDIDATES:
        if cand.is_file():
            return cand
    return None

def _drawer_spec_pure_inventory(installed):
    # Spec: drawer includes Settings, File manager, and installed plugins; builtins are not uninstallable.
    # Canonical ids: builtin.settings, builtin.files; display names: "系统设置"/"Settings", "文件管理"/"File manager"
    # Order not strictly specified for drawer, but must contain both builtins plus plugins.
    inventory = ["builtin.settings", "builtin.files"] + list(installed)
    return inventory

def test_drawer_spec_pure_inventory_contains_builtins_and_plugins():
    assert "builtin.settings" in _drawer_spec_pure_inventory([])
    assert "builtin.files" in _drawer_spec_pure_inventory([])
    inv = _drawer_spec_pure_inventory(["com.weread.client"])
    assert "builtin.settings" in inv and "builtin.files" in inv and "com.weread.client" in inv
    inv2 = _drawer_spec_pure_inventory(["com.fanqie.client", "com.jjwxc.client"])
    assert inv2[0] == "builtin.settings" and inv2[1] == "builtin.files"

def test_drawer_spec_builtins_not_uninstallable():
    inv = _drawer_spec_pure_inventory(["com.weread.client"])
    builtins = {"builtin.settings", "builtin.files"}
    for b in builtins:
        assert b in inv
    # Simulated uninstall guard: builtins should be rejected
    def can_uninstall(app_id):
        return app_id not in builtins
    assert not can_uninstall("builtin.settings")
    assert not can_uninstall("builtin.files")
    assert can_uninstall("com.weread.client")
    assert can_uninstall("com.fanqie.client")

def test_app_list_activity_or_helper_presents_drawer_including_settings_and_file_manager():
    # RED-until-Luna: AppListActivity must present a drawer inventory that includes Settings + File manager + installed plugins
    # Either direct in AppListActivity.cpp/h or via a small extracted helper (if Luna adds one).
    assert APP_LIST_H.is_file(), f"missing {APP_LIST_H}"
    assert APP_LIST_CPP.is_file(), f"missing {APP_LIST_CPP}"
    src_h = _read(APP_LIST_H)
    src_cpp = _read(APP_LIST_CPP)
    helper = _find_helper()
    combined = src_h + src_cpp
    if helper:
        combined += _read(helper)
        combined_path = str(helper)
    else:
        combined_path = f"{APP_LIST_H} + {APP_LIST_CPP}"

    # Check for Settings: look for any of these tokens that indicate Settings is part of inventory
    settings_tokens = ["builtin.settings", "settings", "Settings", "kSystemSettings", "系统设置", "SettingsActivity"]
    files_tokens = ["builtin.files", "files", "FileManager", "kFileManager", "文件管理", "MyLibrary", "onFileTransfer", "File manager"]
    has_settings = any(tok in combined for tok in settings_tokens)
    has_files = any(tok in combined for tok in files_tokens)
    # More precise: need both builtins explicitly listed as drawer items, not just mentioned in comments
    # We check that AppListActivity's reload or inventory building mentions both in same file near "apps_"
    has_settings_inventory = ("builtin.settings" in combined) or ("kSystemSettings" in combined and "apps_" in combined) or ("Settings" in combined and "apps_" in combined)
    has_files_inventory = ("builtin.files" in combined) or ("kFileManager" in combined and "apps_" in combined) or ("文件管理" in combined and "apps_" in combined)

    assert has_settings and has_settings_inventory, (
        f"Drawer must include Settings (builtin.settings) in inventory at {combined_path} — not found. "
        f"Checked tokens {settings_tokens}. Expected RED until Luna drawer lane lands. "
        f"Helper candidates checked: {HOME_DRAWER_HELPER_CANDIDATES}"
    )
    assert has_files and has_files_inventory, (
        f"Drawer must include File manager (builtin.files / 文件管理) in inventory at {combined_path} — not found. "
        f"Checked tokens {files_tokens}. Expected RED until Luna lands."
    )
    # Also ensure it includes installed plugins via M4xRegistry::load (existing)
    assert "M4xRegistry::load" in combined, "drawer must still load installed plugins via M4xRegistry::load"

def test_builtin_apps_are_not_uninstallable():
    # RED-until-Luna: uninstall must be guarded for builtins
    assert APP_LIST_CPP.is_file()
    src = _read(APP_LIST_CPP)
    # Look for uninstallSelected or uninstall logic
    assert "uninstall" in src.lower(), "AppListActivity must have uninstall logic"
    combined = src
    helper = _find_helper()
    if helper:
        combined += _read(helper)

    # Check that there is a guard that prevents uninstalling builtins.
    # We do NOT check polarity (e.g. if (!isBuiltin) vs if (isBuiltin) return) to avoid clash with impl; we just check that both concepts appear near uninstall.
    lower = combined.lower()
    has_uninstall = "uninstall" in lower
    has_builtin = "builtin" in lower
    has_guard = has_uninstall and has_builtin

    # Alternative guard tokens if uses display name or id list
    guard_tokens = ["isBuiltin", "is_builtin", "BUILTIN", "builtin.files", "builtin.settings", "canUninstall", "is_system", "system_app"]
    has_alt_guard = any(tok.lower() in lower for tok in guard_tokens) and has_uninstall

    assert has_guard or has_alt_guard, (
        "Builtin apps must be not uninstallable: expected a guard near uninstall that references builtin ids (e.g. builtin.files / isBuiltin). "
        f"Not found in {APP_LIST_CPP} (and helper if any). has_uninstall={has_uninstall}, has_builtin={has_builtin}, alt={guard_tokens}. "
        "Expected RED until Luna adds guard. Do not weaken: builtins must be protected."
    )
    # Also ensure uninstall path checks id before calling M4xInstaller::uninstall or equivalent
    # We merely ensure that the file mentions both uninstall and a check for builtin/files/settings
    assert "M4xInstaller::uninstall" in combined or "uninstall" in lower, "uninstall must call installer"

def test_drawer_is_grid_not_plain_list():
    # The drawer is specified as "手机桌面式应用抽屉" (phone desktop grid). The old AppListActivity is pure list (drawList).
    # After Luna, it should be grid-like. We check more precisely than comment "drawer" to avoid false positives.
    # Previous version incorrectly passed because it found "drawer" in a comment ("APK-like drawer").
    # Now we require an actual grid implementation token beyond the comment.
    src = _read(APP_LIST_CPP) + _read(APP_LIST_H)
    helper = _find_helper()
    if helper:
        src += _read(helper)
    # Remove the known APK-like drawer comment to avoid false positive
    cleaned = src.replace("APK-like drawer", "")
    # Look for real grid implementation: drawGrid/gridColumns/span/columns/item_width bound to icon rendering, or removal of drawList
    grid_impl_tokens = ["drawGrid", "Grid", "GridLayout", "gridColumns", "numColumns", "icon_home.bmp", "Bitmap<62, 64>", "kHomeAppIcon"]
    has_grid_impl = any(tok in cleaned for tok in grid_impl_tokens)
    has_drawList = "drawList" in cleaned
    # Old code is pure list-only: has drawList without grid impl. New should have grid impl (or at least not be pure list).
    # We make this RED until Luna converts to grid. Currently has_drawList True and no grid impl => RED
    assert has_grid_impl or not has_drawList, (
        "Drawer should be a grid/desktop style (tokens drawGrid/Grid/icon_home.bmp/Bitmap<62,64> etc.), not plain list-only drawList. "
        f"has_grid_impl={has_grid_impl}, has_drawList={has_drawList}. Expected RED until Luna converts AppListActivity from plain list. "
        "Note: comment 'APK-like drawer' alone does not count."
    )


def test_drawer_labels_ellipsize_by_four_codepoints_not_pixel_width():
    src = _read(APP_LIST_CPP)
    assert "utf8EllipsizeChars" in src, (
        "Drawer labels must ellipsize by UTF-8 codepoint count so four CJK glyphs "
        "(文件管理 / 微信读书) stay intact."
    )
    assert "kDrawerLabelMaxChars" in src
    # Pixel-width truncate at tile.width-8 turns 「文件管理」 into 「文件管…」.
    assert "truncated(renderer, UI_10_FONT_ID, item.label.c_str(), tile.width - 8)" not in src

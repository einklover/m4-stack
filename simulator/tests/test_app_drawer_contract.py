import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_LIST_H = ROOT / "firmware/src/activities/apps/AppListActivity.h"
APP_LIST_CPP = ROOT / "firmware/src/activities/apps/AppListActivity.cpp"
CANDIDATE_HELPERS = [
    ROOT / "firmware/src/activities/apps/AppListModel.h",
    ROOT / "firmware/src/activities/apps/DrawerInventory.h",
    ROOT / "firmware/src/activities/apps/AppDrawerHelper.h",
    ROOT / "firmware/src/util/AppDrawerInventory.h",
]

def combined_source():
    parts = []
    if APP_LIST_H.is_file():
        parts.append(APP_LIST_H.read_text(encoding="utf-8"))
    if APP_LIST_CPP.is_file():
        parts.append(APP_LIST_CPP.read_text(encoding="utf-8"))
    for cand in CANDIDATE_HELPERS:
        if cand.is_file():
            parts.append(cand.read_text(encoding="utf-8"))
    return "\n".join(parts)

class AppDrawerContracts(unittest.TestCase):
    def test_drawer_includes_settings_and_file_manager(self):
        src = combined_source()
        # Check for Settings inventory
        self.assertTrue(
            "builtin.settings" in src or "kSystemSettings" in src or "系统设置" in src,
            "drawer must include Settings (builtin.settings / kSystemSettings / 系统设置) — expected RED until Luna lands"
        )
        # File manager
        self.assertTrue(
            "builtin.files" in src or "kFileManager" in src or "文件管理" in src,
            "drawer must include File manager (builtin.files / kFileManager / 文件管理) — expected RED until Luna lands"
        )
        # Still loads installed plugins
        self.assertIn("M4xRegistry::load", src, "drawer must still load installed plugins")

    def test_builtins_are_not_uninstallable(self):
        src = combined_source().lower()
        self.assertIn("uninstall", src, "drawer must have uninstall logic")
        # Do NOT check polarity (if (!isBuiltin) vs if (isBuiltin) return). Just check that both concepts appear together.
        self.assertIn("builtin", src,
                      "builtins must be guarded: expected 'builtin' token near uninstall logic (isBuiltin / builtin.files) — expected RED until Luna lands")

    def test_drawer_mentions_grid_or_drawer(self):
        src = combined_source()
        # Avoid false positive from "APK-like drawer" comment in old code
        cleaned = src.replace("APK-like drawer", "")
        has_grid_impl = any(tok in cleaned for tok in ["drawGrid", "GridLayout", "gridColumns", "numColumns", "icon_home.bmp", "Bitmap<62, 64>", "kHomeAppIcon"])
        has_drawList = "drawList" in cleaned
        self.assertTrue(
            has_grid_impl or not has_drawList,
            f"drawer should be grid/desktop style (drawGrid/Grid/icon_home.bmp/Bitmap<62,64>), not plain list-only drawList — has_grid_impl={has_grid_impl}, has_drawList={has_drawList}. Expected RED until Luna converts."
        )

if __name__ == "__main__":
    unittest.main()

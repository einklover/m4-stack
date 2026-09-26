"""First-boot Home + native App Store source contracts (no hardware needed)."""
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
src = lambda name: (ROOT / name).read_text(encoding="utf-8")


class FirstBootAndStoreContracts(unittest.TestCase):
    def test_four_real_builtin_defaults_honor_saved_pins(self):
        dock = src("firmware/src/apps/M4HomeDock.h")
        for app in ("builtin.files", "builtin.transfer", "builtin.store", "builtin.settings"):
            self.assertIn(f'"{app}"', dock)
        self.assertIn("Honor exact pinned positions", dock)
        self.assertIn("place(i, pinned[", dock)
        self.assertIn("kSlotCount = 4", dock)

    def test_placeholder_covers_exist_without_inventing_recent_files(self):
        home = src("firmware/src/activities/home/HomeActivity.cpp")
        self.assertIn("if (ctx.recentBooks.empty())", home)
        self.assertIn("addFirstBootArtwork(ctx.model.draftPublication())", home)
        self.assertIn('ctx.model.addRecent("导入书籍"', home)
        self.assertIn("if (path.empty()) onMyLibraryOpen()", home)
        self.assertNotIn("RecentBooksStore::save", home)
        self.assertIn("HomeScene::homeAddAssetToPublication", home)

    def test_both_store_navigation_surfaces_are_wired(self):
        home = src("firmware/src/activities/home/HomeActivity.cpp")
        drawer = src("firmware/src/activities/apps/AppListActivity.cpp")
        main = src("firmware/src/main.cpp")
        self.assertIn('id == "builtin.store"', home)
        self.assertIn("BuiltinAction::AppStore", drawer)
        self.assertIn("callbacks.onAppStoreOpen", main)
        self.assertIn("new AppStoreActivity", main)

    def test_store_has_bounded_catalog_and_sha_verified_packages(self):
        store = src("firmware/src/activities/apps/AppStoreActivity.cpp")
        self.assertIn("kMaxCatalogBytes", store)
        self.assertIn("kMaxPackageBytes", store)
        self.assertIn("M4HttpTransport::requestToSink", store)
        self.assertIn("M4NativeWifi::ensureConnected", store)
        self.assertIn("readCache", store)
        self.assertIn("mbedtls_sha256_update(", store)
        self.assertIn("digest == app.sha256", store)
        self.assertIn("probe.manifest.id != app.id", store)
        self.assertIn("probe.manifest.versionCode != app.versionCode", store)
        self.assertIn("M4xInstaller::probe", store)
        self.assertIn("new AppInstallActivity", store)

    def test_catalog_manifest_contract(self):
        catalog = json.loads(src("docs/appstore/index.json"))
        self.assertEqual(catalog["schemaVersion"], 1)
        ids = set()
        for app in catalog["apps"]:
            self.assertNotIn(app["id"], ids)
            ids.add(app["id"])
            self.assertGreater(app["versionCode"], 0)
            self.assertRegex(app["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(app["packageUrl"].startswith(
                "https://einklover.github.io/m4-stack/appstore/packages/"))
            self.assertTrue(app["sourceUrl"].startswith("https://github.com/einklover/m4-stack/"))


if __name__ == "__main__":
    unittest.main()

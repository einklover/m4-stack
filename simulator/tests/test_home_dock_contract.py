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
        for virtual_cover in ("firstboot:hero", "firstboot:import", "firstboot:read", "firstboot:shelf"):
            self.assertIn(virtual_cover, home, "empty refs hide procedural artwork in QEMU")
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

    def test_store_has_visible_wifi_refresh_and_pagination_touch_controls(self):
        store = src("firmware/src/activities/apps/AppStoreActivity.cpp")
        header = src("firmware/src/activities/apps/AppStoreActivity.h")
        main = src("firmware/src/main.cpp")
        self.assertIn("onWifiOpen_", header)
        self.assertIn("onGoToAppStoreWifi", main)
        self.assertIn("M4WifiSelectionPurpose::SystemNetworking", main)
        self.assertIn("kToolbarTop", store)
        self.assertIn('"Wi-Fi 设置"', store)
        self.assertIn('"刷新目录"', store)
        self.assertIn('"上一页"', store)
        self.assertIn('"下一页 "', store)
        self.assertIn('y >= kToolbarTop && y < kToolbarTop + kToolbarHeight', store)
        self.assertIn("shown_.size()", store)
        self.assertIn('"刷新失败：" + error', store)
        # Toolbar is outside of the non-empty-list branch: no network still offers Wi-Fi.
        self.assertLess(store.index('renderer.drawRoundedRect(18,kToolbarTop'),
                        store.index('if (shown_.empty()) {', store.index('void AppStoreActivity::render()')))

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


    def test_catalog_includes_every_plugin_at_source_version(self):
        """Adding a plugin or bumping a manifest must update the public catalog."""
        catalog = json.loads(src("docs/appstore/index.json"))
        manifests = {}
        for path in (ROOT / "plugins").glob("*/manifest.json"):
            entry = json.loads(path.read_text(encoding="utf-8"))
            self.assertNotIn(entry["id"], manifests)
            manifests[entry["id"]] = entry
        self.assertEqual(len(manifests), 15)
        self.assertEqual({app["id"] for app in catalog["apps"]}, set(manifests))
        for app in catalog["apps"]:
            source = manifests[app["id"]]
            self.assertEqual((app["version"], app["versionCode"]),
                             (source["version"], source["versionCode"]))
            self.assertIn(f'-v{source["version"]}.m4x', app["packageUrl"])
        self.assertLess(len(src("docs/appstore/index.json").encode("utf-8")), 48 * 1024)

    def test_legado_manifest_does_not_publish_local_test_endpoint(self):
        manifest = json.loads(src("plugins/m4-legado-plugin/manifest.json"))
        self.assertNotIn("192.168.0.118:1122", manifest["description"])



if __name__ == "__main__":
    unittest.main()

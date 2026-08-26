import importlib.util
import tempfile
import unittest
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class NativeProviderUiContractTests(unittest.TestCase):
    def test_provider_entries_have_direct_book_activation_and_no_details_footer(self):
        for provider in ("fanqie", "jjwxc", "weread", "legado"):
            plugin = ROOT / "plugins" / f"m4-{provider}-plugin"
            root = ET.parse(plugin / "main.xml").getroot()
            home = root.find("./screen[@id='home']")
            self.assertIsNotNone(home, provider)
            books = home.find("./list[@id='books']")
            self.assertIsNotNone(books, provider)
            self.assertEqual(books.attrib.get("onActivate"), "provider.openBook")
            buttons = home.find("./buttons")
            self.assertIsNotNone(buttons, provider)
            self.assertNotEqual(buttons.attrib.get("primary"), "详情")

    def test_native_activity_tap_dispatches_list_action_directly(self):
        source = (ROOT / "firmware/src/activities/apps/NativeAppActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("handleAction(listAction_, nullptr, hit)", source)
        self.assertIn("handleAction(listAction_.empty() ? buttonActions_[1] : listAction_, nullptr, selectedIndex_)", source)
        self.assertIn("footerLayout_.buttonAt", source)

    def test_detail_enrichment_is_not_run_by_activity_task(self):
        source = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp").read_text()
        self.assertNotIn("M4NativeProviderBookDetail::fetch(req)", source)
        self.assertNotIn("M4ProviderCoverCache::acquireProviderCover", source)
        self.assertIn(
            "M4NativeProviderBookDetailAsync::start(req, metrics.homeCoverWidth, metrics.homeCoverThumbHeight)",
            source,
        )
        self.assertIn("pollDetailLoading", source)
        self.assertIn("detailError_", source)

    def test_provider_cover_enrichment_runs_in_provider_worker(self):
        activity = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp").read_text()
        async_worker = (ROOT / "firmware/src/apps/providers/M4NativeProviderBookDetailAsync.cpp").read_text()
        cover = (ROOT / "firmware/src/util/M4ProviderCoverCache.cpp").read_text()
        self.assertNotIn("M4ProviderCoverCache::acquireProviderCover", activity)
        self.assertIn("M4ProviderCoverCache::acquireProviderCover", async_worker)
        self.assertIn("publish(result.ok ? Phase::Ready : Phase::Error", async_worker)
        self.assertIn("coverRequest.cancelled = cancelled", async_worker)
        self.assertIn("constexpr uint32_t kCoverSettleMs = 650u", async_worker)
        self.assertIn("waitForCoverSettle()", async_worker)
        self.assertLess(async_worker.index("waitForCoverSettle()"),
                        async_worker.index("M4ProviderCoverCache::acquireProviderCover"))
        self.assertNotIn("UITheme::getInstance().getMetrics()", async_worker)
        self.assertIn("int homeCoverWidth, int homeCoverThumbHeight", async_worker)
        self.assertIn("snap.coverBmpPath", activity)
        self.assertNotIn("HttpDownloader", cover)
        self.assertIn("M4NativeProviderHttp::requestToSink", cover)

    def test_provider_cover_sink_accepts_empty_chunks(self):
        cover = (ROOT / "firmware/src/util/M4ProviderCoverCache.cpp").read_text()
        self.assertIn("if (len == 0) return true;", cover)
        self.assertIn("if (failed_ || !data) return false;", cover)

    def test_detail_has_single_back_footer_and_geometry_driven_touch_targets(self):
        source = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp").read_text()
        header = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.h").read_text()
        self.assertIn("state_ == State::Detail", header)
        self.assertIn("? M4FooterTouchPolicy::Back", header)
        self.assertIn('mapLabels("« 返回", "", "", "")', source)
        self.assertIn("detailTouch_.reset", source)
        self.assertIn("detailTouch_.setChapterBlock", source)
        self.assertIn("detailTouch_.actionAt", source)
        self.assertIn("openToc();", source)
        self.assertNotIn("tx * 4 /", source)

    def test_catalog_and_chapter_handoffs_do_not_sleep_ui_task(self):
        source = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp").read_text()
        self.assertNotIn("delay(600)", source)
        self.assertIn("catalogStartPending_", source)
        self.assertIn("chapterStartPending_", source)

    def test_chapter_loading_has_a_terminal_timeout(self):
        source = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp").read_text()
        self.assertIn("kChapterLoadingTimeoutMs", source)
        self.assertIn('"chapter_timeout"', source)
        self.assertIn("cancelForeground()", source)

    def test_file_row_chapter_queue_does_not_resolve_from_row_zero_on_ui_task(self):
        source = (ROOT / "firmware/src/apps/providers/M4NativeProviderManager.cpp").read_text()
        request = source[source.index("bool requestChapter("):source.index("bool ensureChapter(")]
        file_rows = request.split("M4ContentProvider::ChapterMeta ch;", 1)[0]
        self.assertIn("ChapterCatalogKind::FileRows", request)
        self.assertIn("requestPrefetch(providerId, bookId, index0)", file_rows)
        self.assertNotIn("resolveChapter(b, index0", file_rows)
        self.assertIn("kLegacyUiScanMaxBytes", source)

    def test_login_start_failure_has_retry_feedback(self):
        source = (ROOT / "firmware/src/activities/apps/NativeProviderLoginActivity.cpp").read_text()
        self.assertIn("startFailed_", source)
        self.assertIn("上一项登录仍在结束，请稍后重试", source)
        self.assertIn("确认键重试", source)

    def test_shelf_index_is_capped_and_pumped(self):
        source = (ROOT / "firmware/src/apps/native/M4NativeAppControllerFactory.cpp").read_text()
        self.assertNotIn("buildShelfIndex(", source)
        self.assertNotIn("projectLegacyShelf(", source)
        self.assertIn("pumpShelfIndex", source)
        self.assertIn("kMaxShelfBytes", source)
        self.assertIn("kMaxShelfRows", source)

    def test_jjwxc_source_package_derives_uncommitted_gbk_asset(self):
        package_path = ROOT / "firmware/scripts/m4adb_lib/package.py"
        spec = importlib.util.spec_from_file_location("jjwxc_package", package_path)
        package = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(package)

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "com.jjwxc.client.m4x"
            package.build_m4x(ROOT / "plugins/m4-jjwxc-plugin", output)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.getinfo("gbk_table.bin").file_size, 126 * 190 * 2)
                self.assertIn("main.xml", archive.namelist())


if __name__ == "__main__":
    unittest.main()

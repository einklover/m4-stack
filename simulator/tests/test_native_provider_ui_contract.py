import importlib.util
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class NativeProviderUiContractTests(unittest.TestCase):
    def test_detail_enrichment_is_not_run_by_activity_task(self):
        source = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp").read_text()
        self.assertNotIn("M4NativeProviderBookDetail::fetch(req)", source)
        self.assertIn("M4NativeProviderBookDetailAsync::start(req)", source)
        self.assertIn("pollDetailLoading", source)
        self.assertIn("detailError_", source)

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

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DISCOVERY = ROOT / "firmware/src/apps/providers/M4NativeProviderDiscovery.cpp"
CONTROLLER = ROOT / "firmware/src/apps/native/M4NativeAppControllerFactory.cpp"
IO = ROOT / "firmware/src/apps/providers/M4NativeProviderIo.cpp"
SCHEMA = ROOT / "firmware/src/apps/providers/M4ProviderShelfCache.h"


class ProviderShelfCacheContractTests(unittest.TestCase):
    def test_discovery_uses_schema_columns_as_extractor_source(self):
        schema = SCHEMA.read_text()
        discovery = DISCOVERY.read_text()
        for columns in (
            '"book_id", "book_name", "author", "_m4_progress", "thumb_url"',
            '"novelId", "novelName", "authorName", "_m4_progress", "cover"',
            '"bookId", "title", "author", "progress", "cover"',
            '"bookUrl", "name", "author", "totalChapterNum", "latestChapterTitle", "coverUrl"',
        ):
            self.assertIn(columns, schema)
        self.assertEqual(discovery.count("s.fields = shelfSchema->columns;"), 4)
        self.assertNotIn("s.fields = {", discovery)

    def test_invalid_cache_is_empty_and_auto_refresh_is_flag_driven(self):
        controller = CONTROLLER.read_text()
        self.assertIn("shelfCacheNeedsRefresh_ = true;", controller)
        self.assertIn("shelfCacheNeedsRefresh_ = false;", controller)
        self.assertIn("shouldAutoDiscover(shelfCacheNeedsRefresh_", controller)
        self.assertNotIn(
            "shelfCount_ != 0 || autoDiscoveryAttempted_ || M4NativeProviderDiscovery::busy()",
            controller,
        )
        self.assertIn("metadataPath(shelfRows_)", controller)
        self.assertIn("recoverTempFilesPair", controller)

    def test_pair_commit_has_distinct_backups_and_rollback(self):
        io = IO.read_text()
        self.assertIn('replacedExtension(firstFinal, "rkb")', io)
        self.assertIn('replacedExtension(secondFinal, "mkb")', io)
        self.assertIn("firstInstalled", io)
        self.assertIn("secondInstalled", io)
        self.assertIn("commitTempFilesPair", io)
        self.assertIn("recoverTempFilesPair", io)
        self.assertIn("stageEmpty", DISCOVERY.read_text())
        self.assertIn('rows.recordCount() > 0 || job.providerId == "weread"', DISCOVERY.read_text())

    def test_legacy_fanqie_without_meta_is_an_explicit_migration(self):
        schema = SCHEMA.read_text()
        self.assertIn("cacheNeedsRefresh", schema)
        self.assertIn('"thumb_url"', schema)
        self.assertIn('cacheNeedsRefresh("fanqie", true, "")',
                      (ROOT / "firmware/tests/native_app/test_provider_shelf_cache.cpp").read_text())


if __name__ == "__main__":
    unittest.main()

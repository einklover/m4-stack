"""Focused contracts for the Fanqie discovery network path."""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/src/apps/providers/M4NativeProviderDiscovery.cpp"


class FanqieNetworkContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_fanqie_shelf_open_is_deferred_until_response_body(self):
        source = self.source
        self.assertIn('file.open(rowsPath(job.appId), job.providerId == "fanqie")', source)
        self.assertIn("if (deferFileOpen) return true;", source)
        self.assertIn("if (!data || !ensureFile()) return false;", source)

        fanqie_open = source.index('file.open(rowsPath(job.appId), job.providerId == "fanqie")')
        first_http_request = source.index("const auto net = M4NativeProviderHttp::requestToSink", fanqie_open)
        http_request = source.index("const auto net = M4NativeProviderHttp::requestToSink", first_http_request + 1)
        self.assertLess(fanqie_open, http_request)

        request_block = source[fanqie_open:http_request]
        self.assertIn('if (job.providerId != "fanqie")', request_block)
        self.assertIn('writeDiscoveryDiag(job.appId, "http_started"', request_block)
        request_result = source[http_request:source.index("const bool boundedWindow", http_request)]
        self.assertIn('writeDiscoveryDiag(job.appId, "http_progress"', request_result)

    def test_fanqie_request_remains_bounded_and_streaming(self):
        source = self.source
        fanqie_spec = source[source.index('if (providerId == "fanqie")'):source.index('if (providerId == "jjwxc")')]
        self.assertIn("novel.snssdk.com/api/novel/channel/homepage/new_category/book_list/v1/", fanqie_spec)
        self.assertIn('s.path = {"data", "data"};', fanqie_spec)
        self.assertIn('s.request.maxBytes = 4u * 1024u * 1024u;', fanqie_spec)
        self.assertIn('s.maxRows = 24;', fanqie_spec)
        self.assertIn('"book_id", "book_name", "author", "_m4_progress"', fanqie_spec)

    def test_chapter_header_failure_gets_one_clean_retry(self):
        source = (ROOT / "firmware/src/apps/providers/FanqieProvider.cpp").read_text(encoding="utf-8")
        self.assertIn("kMaxChapterAttempts", source)
        self.assertIn("http_ESP_ERR_HTTP_FETCH_HEADER", source)
        self.assertIn("removeIncomplete(req.cacheAbsPath)", source)

    def test_discovery_shelf_coalesces_tsv_writes(self):
        source = self.source
        sink = source[source.index("class AtomicRowsSink"):source.index("class RecordExtractorSink")]
        self.assertIn("kBufferBytes", sink)
        self.assertIn("flushBuffer", sink)
        self.assertIn("used_", sink)



if __name__ == "__main__":
    unittest.main()

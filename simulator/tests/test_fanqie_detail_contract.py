"""Host regressions for the current Fanqie book-detail response contract."""

import html
import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/src/apps/providers/M4NativeProviderBookDetail.cpp"
FIXTURE = ROOT / "firmware/tests/native_app/fixtures/fanqie_detail.json"


def clean_intro(value: str, max_bytes: int = 1536) -> str:
    """Mirror the provider's bounded plain-text normalization contract."""
    value = re.sub(r"<[^>]*>", " ", value)
    value = html.unescape(value)
    value = re.sub(r"\s+", " ", value).strip()
    if len(value.encode("utf-8")) <= max_bytes:
        return value
    encoded = value.encode("utf-8")[:max_bytes]
    while encoded and (encoded[-1] & 0xC0) == 0x80:
        encoded = encoded[:-1]
    return encoded.decode("utf-8")


def project_detail(payload: dict, seed_title: str = "") -> dict:
    """Model the provider's bounded data-node projection for host assertions."""
    data = payload.get("data")
    node = data if isinstance(data, dict) else (data[0] if data else {})
    return {
        "title": node.get("bookName") or node.get("book_name") or node.get("title") or seed_title,
        "author": node.get("author") or node.get("authorName") or node.get("author_name") or "",
        "coverUrl": node.get("thumbUrl") or node.get("thumb_url") or "",
        "intro": clean_intro(node.get("abstract") or node.get("introduction") or node.get("intro") or ""),
        "lastChapter": node.get("lastChapterTitle") or node.get("last_chapter_title") or "",
    }


class FanqieDetailContractTests(unittest.TestCase):
    def test_current_api_fixture_extracts_intro_and_keeps_metadata(self):
        payload = json.loads(FIXTURE.read_text(encoding="utf-8"))
        detail = project_detail(payload)

        self.assertEqual(detail["title"], "十日终焉")
        self.assertEqual(detail["author"], "杀虫队队员")
        self.assertTrue(detail["coverUrl"])
        self.assertIn("24年番茄年度巅峰榜TOP1", detail["intro"])
        self.assertIn("终焉之地", detail["intro"])
        self.assertEqual(detail["lastChapter"], "陈俊南（终）")

        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("https://fanqienovel.com/api/book/info?bookId=", source)
        self.assertIn('JsonObjectConst object = doc["data"].as<JsonObjectConst>();', source)
        self.assertIn('firstText(node, {"abstract", "introduction", "intro"})', source)

    def test_intro_normalization_removes_html_entities_and_bounds_output(self):
        self.assertEqual(clean_intro("<p>第一段&nbsp;&amp; 第二段</p>\n\t第三段"), "第一段 & 第二段 第三段")
        self.assertEqual(clean_intro("x" * 2000, max_bytes=5), "xxxxx")

    def test_missing_or_empty_intro_is_non_fatal(self):
        for payload in (
            {"data": {"bookName": "无简介之书", "author": "作者", "abstract": ""}},
            {"data": {"bookName": "无字段之书", "author": "作者"}},
            {"data": {"bookName": "种子标题"}},
        ):
            detail = project_detail(payload, seed_title="种子标题")
            self.assertTrue(detail["title"])
            self.assertEqual(detail["coverUrl"], "")
            self.assertEqual(detail["intro"], "")


if __name__ == "__main__":
    unittest.main()

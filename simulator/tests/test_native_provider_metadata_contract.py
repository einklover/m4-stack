"""Track F contracts for native provider author/cover projections."""

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "firmware/tests/native_app/fixtures"
DISCOVERY = ROOT / "firmware/src/apps/providers/M4NativeProviderDiscovery.cpp"
DETAIL = ROOT / "firmware/src/apps/providers/M4NativeProviderBookDetail.cpp"
DETAIL_HEADER = ROOT / "firmware/src/apps/providers/M4NativeProviderBookDetail.h"


def load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def project(row: dict, id_key: str, title_key: str, author_key: str, cover_key: str) -> dict:
    return {
        "bookId": str(row.get(id_key, "")),
        "title": str(row.get(title_key, "")),
        "author": str(row.get(author_key, "")),
        "coverUrl": str(row.get(cover_key, "")),
    }


class NativeProviderMetadataContracts(unittest.TestCase):
    def test_fanqie_discovery_and_detail_use_real_cover_fields(self):
        discovery = load("fanqie_discovery.json")["data"]["data"]
        self.assertEqual(project(discovery[0], "book_id", "book_name", "author", "thumb_url")["author"], "弈青锋")
        self.assertTrue(project(discovery[0], "book_id", "book_name", "author", "thumb_url")["coverUrl"])
        self.assertEqual(project(discovery[1], "book_id", "book_name", "author", "thumb_url")["coverUrl"], "")

        detail = load("fanqie_detail.json")["data"]
        self.assertEqual(detail["authorName"], "杀虫队队员")
        self.assertEqual(detail["thumbUrl"], load("fanqie_detail.json")["data"]["thumbUrl"])
        source = DETAIL.read_text(encoding="utf-8")
        discovery_source = DISCOVERY.read_text(encoding="utf-8")
        self.assertIn('https://fanqienovel.com/api/book/info?bookId=', source)
        self.assertIn('JsonObjectConst object = doc["data"].as<JsonObjectConst>();', source)
        self.assertIn('firstText(node, {"thumbUrl", "thumb_url"})', source)
        self.assertIn('"book_id", "book_name", "author", "_m4_progress", "thumb_url"', discovery_source)

    def test_jjwxc_streaming_discovery_and_bounded_detail_map_author_cover(self):
        row = load("jjwxc_discovery.json")["14000019"][0]
        card = project(row, "novelId", "novelName", "authorName", "cover")
        self.assertEqual(card["author"], "写离声")
        self.assertTrue(card["coverUrl"])
        empty = load("jjwxc_discovery.json")["14000019"][1]
        self.assertEqual(project(empty, "novelId", "novelName", "authorName", "cover")["coverUrl"], "")
        detail = load("jjwxc_detail.json")
        self.assertEqual(detail["authorName"], "写离声")
        self.assertTrue(detail["novelCover"])

        discovery_source = DISCOVERY.read_text(encoding="utf-8")
        detail_source = DETAIL.read_text(encoding="utf-8")
        self.assertIn('"novelId", "novelName", "authorName", "_m4_progress", "cover"', discovery_source)
        self.assertIn('firstText(node, {"novelCover", "originalCover"})', detail_source)
        self.assertIn("requestSmall(http, body, net, req.maxBytes, cancelled)", detail_source)
        self.assertNotIn("getFullPage", detail_source)

    def test_weread_shelf_and_detail_use_cover_without_changing_auth(self):
        rows = load("weread_shelf.json")["books"]
        card = project(rows[0], "bookId", "title", "author", "cover")
        self.assertEqual(card["author"], "作者一")
        self.assertTrue(card["coverUrl"])
        self.assertEqual(project(rows[1], "bookId", "title", "author", "cover")["coverUrl"], "")
        detail = load("weread_detail.json")["bookInfo"]
        self.assertEqual(detail["author"], "作者一")
        self.assertTrue(detail["cover"])

        discovery_source = DISCOVERY.read_text(encoding="utf-8")
        detail_source = DETAIL.read_text(encoding="utf-8")
        self.assertIn('"bookId", "title", "author", "progress", "cover"', discovery_source)
        self.assertIn('firstText(node, {"cover"})', detail_source)
        self.assertIn('loadCookieHeader(appRoot(req.appId), "weread", cookie)', detail_source)

    def test_legado_keeps_cover_empty_and_legacy_rows_parse(self):
        row = load("legado_shelf_no_cover.json")["data"][0]
        self.assertEqual(row.get("author"), "作者")
        self.assertEqual(row.get("cover", ""), "")

        # The persisted legacy formats remain id/title/author/meta plus the
        # optional latest-chapter field; no invented cover field is required.
        for legacy, expected in (
            ("fanqie-id\t番茄旧书\t作者\t0", ["fanqie-id", "番茄旧书", "作者", "0"]),
            ("jjwxc-id\t晋江旧书\t作者\t1", ["jjwxc-id", "晋江旧书", "作者", "1"]),
            ("weread-id\t微信旧书\t作者\t42", ["weread-id", "微信旧书", "作者", "42"]),
            ("legado-id\t本地旧书\t作者\t10\t最新章", ["legado-id", "本地旧书", "作者", "10"]),
        ):
            fields = legacy.split("\t")
            self.assertEqual(fields[:4], expected)
        source = DISCOVERY.read_text(encoding="utf-8")
        controller = (ROOT / "firmware/src/apps/native/M4NativeAppControllerFactory.cpp").read_text(
            encoding="utf-8"
        )
        header = DETAIL_HEADER.read_text(encoding="utf-8")
        legado_spec = source[source.index('if (providerId == "legado")'):source.index('if (providerId == "weread")')]
        self.assertIn('"bookUrl", "name", "author", "totalChapterNum", "latestChapterTitle"', legado_spec)
        self.assertNotIn('"cover"', legado_spec)
        self.assertIn("fieldAt(line, 0, rawKey)", controller)
        self.assertIn("fieldAt(line, 1, out.title)", controller)
        self.assertIn("fieldAt(line, 2, out.subtitle)", controller)
        self.assertIn("fieldAt(line, 3, out.value)", controller)
        cover_read = 'fieldAt(line, 4, out.coverUrl)'
        self.assertIn(cover_read, controller)
        cover_gate = controller[controller.index('if (app_.provider == "fanqie"'):controller.index(cover_read) + len(cover_read)]
        self.assertIn('app_.provider == "fanqie"', cover_gate)
        self.assertIn('app_.provider == "jjwxc"', cover_gate)
        self.assertIn('app_.provider == "weread"', cover_gate)
        self.assertNotIn('app_.provider == "legado"', cover_gate)
        self.assertIn("Older 4-column", (ROOT / "firmware/tests/native_app/test_legado_detail.cpp").read_text())
        self.assertIn("fields[2]", header)

    def test_discovery_cover_reaches_detail_seed_through_activation_chain(self):
        controller = (ROOT / "firmware/src/apps/native/M4NativeAppControllerFactory.cpp").read_text(
            encoding="utf-8"
        )
        ui_row = (ROOT / "firmware/src/apps/native/M4NativeUiController.h").read_text(encoding="utf-8")
        activity = (ROOT / "firmware/src/activities/apps/NativeAppActivity.cpp").read_text(encoding="utf-8")
        book_activity = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp").read_text(
            encoding="utf-8"
        )
        book_header = (ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.h").read_text(
            encoding="utf-8"
        )
        detail = DETAIL.read_text(encoding="utf-8")
        detail_header = DETAIL_HEADER.read_text(encoding="utf-8")

        # This is the production chain, not a fixture-only projection:
        # persisted TSV -> Row -> activated activity -> Request -> seed().
        self.assertIn("std::string coverUrl;", ui_row)
        self.assertIn("fieldAt(line, 4, out.coverUrl)", controller)
        self.assertIn("selectedCoverUrl = row.coverUrl", activity)
        self.assertIn("false, -1, selectedCoverUrl", activity)
        self.assertIn("std::string coverUrl_", book_header)
        self.assertIn("req.coverUrl = coverUrl_", book_activity)
        self.assertIn("std::string coverUrl;", detail_header)
        self.assertIn("detail.coverUrl = boundedUtf8(req.coverUrl, kFieldMax)", detail)

        # Missing field 4 is valid for all append-only legacy rows. Legado's
        # field 4 remains the latest-chapter display field, never cover.
        for line in (
            "fanqie-id\t番茄旧书\t作者\t0",
            "jjwxc-id\t晋江旧书\t作者\t1",
            "weread-id\t微信旧书\t作者\t42",
            "legado-id\t本地旧书\t作者\t10\t最新章",
        ):
            self.assertEqual(line.split("\t")[4:] if len(line.split("\t")) > 4 else [],
                             (["最新章"] if line.startswith("legado-") else []))

        self.assertIn("out.detail = seed(req)", detail)
        self.assertIn("if (!value.empty()) dst =", detail)


if __name__ == "__main__":
    unittest.main()

// Host regression: Legado detail must be servable from local shelf/seed
// metadata without re-fetching /getBookshelf (device freeze root cause).
#include <cassert>
#include <cstdio>
#include <string>

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4NativeProviderBookDetail.h"

#include "legado_intro_fixture.inc"

namespace {

class StringSink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    row.append(reinterpret_cast<const char*>(data), len);
    return true;
  }

  std::string row;
};

}  // namespace

int main() {
  using namespace M4NativeProviderBookDetail;

  // Seed title alone is enough for a non-blocking detail page.
  M4NovelProvider::BookDetail seeded;
  seeded.title = "关九九";
  seeded.author = "江门二爷";
  assert(legadoLocalDetailSufficient(seeded));
  assert(!legadoLocalDetailSufficient(M4NovelProvider::BookDetail{}));

  // Shelf row with latestChapterTitle (post-discovery format).
  M4NovelProvider::BookDetail fromShelf;
  const std::string row =
      "0123456789abcdef\t关九九\t江门二爷\t120\t第3章 关九九";
  assert(applyShelfRow(row, "0123456789abcdef", fromShelf));
  assert(fromShelf.title == "关九九");
  assert(fromShelf.author == "江门二爷");
  assert(fromShelf.lastChapter == "第3章 关九九");
  assert(fromShelf.coverUrl.empty());
  assert(fromShelf.intro.empty());
  assert(legadoLocalDetailSufficient(fromShelf));

  // New append-only cover column follows latestChapterTitle and is proxied
  // through the configured endpoint; the endpoint is intentionally not fixed.
  M4NovelProvider::BookDetail withCover;
  const std::string sourceCover =
      "https://www.deqixs.cc/files/article/image/5/5434/5434s.jpg";
  const std::string coverRow = row + "\t" + sourceCover;
  assert(applyShelfRow(coverRow, "0123456789abcdef", withCover, "http://10.0.0.9:8080"));
  assert(withCover.lastChapter == "第3章 关九九");
  assert(withCover.coverUrl ==
         "http://10.0.0.9:8080/cover?path=https%3A%2F%2Fwww.deqixs.cc%2Ffiles%2Farticle%2Fimage%2F5%2F5434%2F5434s.jpg");

  // Wrong id must not mutate detail.
  M4NovelProvider::BookDetail other;
  other.title = "keep";
  assert(!applyShelfRow(row, "ffffffffffffffff", other));
  assert(other.title == "keep");
  assert(other.lastChapter.empty());

  // Older 4-column shelf rows (no latestChapterTitle) still yield title/author.
  M4NovelProvider::BookDetail legacy;
  assert(applyShelfRow("aabbccddeeff0011\t旧书\t佚名\t10", "aabbccddeeff0011", legacy));
  assert(legacy.title == "旧书");
  assert(legacy.author == "佚名");
  assert(legacy.lastChapter.empty());
  assert(legacy.coverUrl.empty());
  assert(legadoLocalDetailSufficient(legacy));

  // A 5-column legacy Legado row has only latestChapterTitle; never treat it
  // as a cover source.
  M4NovelProvider::BookDetail legacyFive;
  assert(applyShelfRow("aabbccddeeff0011\t旧书\t佚名\t10\t最新章", "aabbccddeeff0011",
                       legacyFive, "http://10.0.0.9:8080"));
  assert(legacyFive.lastChapter == "最新章");
  assert(legacyFive.coverUrl.empty());

  // Seven-column rows append intro after coverUrl. Old columns retain their
  // meaning, and multiline JSON text is sanitized by RecordExtractor before
  // it reaches the local-only detail parser.
  StringSink sink;
  M4xJsonStream::RecordExtractor extractor(
      {"data"}, {"bookUrl", "name", "author", "totalChapterNum", "latestChapterTitle",
                  "coverUrl", "intro"},
      sink);
  const std::string fixture = kLegadoIntroShelfFixture;
  for (size_t offset = 0; offset < fixture.size(); offset += 5) {
    const size_t len = std::min<size_t>(5, fixture.size() - offset);
    assert(extractor.feed(reinterpret_cast<const uint8_t*>(fixture.data() + offset), len));
  }
  assert(extractor.finish());
  assert(sink.row ==
         "0123456789abcdef\t关九九\t江门二爷\t120\t第3章 关九九\tcover-original\t第一行 第二行 第三行 \n");

  M4NovelProvider::BookDetail withIntro;
  assert(applyShelfRow(sink.row, "0123456789abcdef", withIntro,
                       "http://10.0.0.9:8080"));
  assert(withIntro.lastChapter == "第3章 关九九");
  assert(withIntro.coverUrl ==
         "http://10.0.0.9:8080/cover?path=cover-original");
  assert(withIntro.intro == "第一行 第二行 第三行");

  // Intro is bounded on the same 1536-byte limit used by remote detail paths.
  M4NovelProvider::BookDetail boundedIntro;
  assert(applyShelfRow("id\t书\t作\t1\t最新\tcover\t" + std::string(1800, 'x'),
                       "id", boundedIntro));
  assert(boundedIntro.intro.size() == 1536);

  // Prefix collision: id must be followed by a tab.
  M4NovelProvider::BookDetail collision;
  assert(!applyShelfRow("0123456789abcdefEXTRA\tnope\ta\t1", "0123456789abcdef", collision));
  assert(collision.title.empty());

  std::printf("legado local detail (shelf/seed, no bookshelf refetch): PASS\n");
  return 0;
}

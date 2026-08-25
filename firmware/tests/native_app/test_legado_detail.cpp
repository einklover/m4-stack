// Host regression: Legado detail must be servable from local shelf/seed
// metadata without re-fetching /getBookshelf (device freeze root cause).
#include <cassert>
#include <cstdio>
#include <string>

#include "apps/providers/M4NativeProviderBookDetail.h"

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
  assert(legadoLocalDetailSufficient(fromShelf));

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

  // Prefix collision: id must be followed by a tab.
  M4NovelProvider::BookDetail collision;
  assert(!applyShelfRow("0123456789abcdefEXTRA\tnope\ta\t1", "0123456789abcdef", collision));
  assert(collision.title.empty());

  std::printf("legado local detail (shelf/seed, no bookshelf refetch): PASS\n");
  return 0;
}

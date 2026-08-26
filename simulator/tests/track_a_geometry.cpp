#include "util/TouchHitGeometry.h"

#include <cassert>

int main() {
  const auto chapterBack = TouchHitGeometry::chapterHeaderBackRect();
  assert(chapterBack.width >= 100);
  assert(chapterBack.height >= 60);
  assert(chapterBack.contains(8, 8));
  assert(chapterBack.contains(chapterBack.width - 1, chapterBack.height - 1));
  assert(!chapterBack.contains(chapterBack.width, 8));
  assert(!chapterBack.contains(8, chapterBack.height));
  const auto offsetChapterBack = TouchHitGeometry::chapterHeaderBackRect(30, 50);
  assert(offsetChapterBack.contains(30, 50));
  assert(!offsetChapterBack.contains(29, 50));
  assert(!offsetChapterBack.contains(30, 49));

  const auto bottom = TouchHitGeometry::makeBottomNavigationLayout(480, 800);
  assert(bottom.valid());
  assert(bottom.back.height >= 48);
  assert(bottom.home.height >= 48);
  assert(bottom.back.contains(100, 775));
  assert(bottom.home.contains(380, 775));
  assert(!bottom.back.contains(380, 775));
  assert(!bottom.home.contains(100, 775));
  return 0;
}

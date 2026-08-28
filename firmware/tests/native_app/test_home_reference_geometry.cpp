#include <assert.h>
#include <cstdio>

#include "util/TouchHitGeometry.h"

int main() {
  using namespace TouchHitGeometry;

  const Rect recent{0, 56, 480, 470};
  const auto layout = makeFengyanRecentLayout(recent, 3, 20);
  assert(layout.valid());
  assert(layout.bookCount == 3);
  assert(layout.hero.width > layout.mini[0].width);
  assert(layout.hero.height > layout.mini[0].height);
  assert(layout.hero.y < layout.mini[0].y);
  assert(layout.mini[0].y == layout.mini[1].y);

  int hit = -1;
  assert(fengyanRecentBookIndexFromPoint(
      recent, 3, 20, layout.hero.x + layout.hero.width / 2,
      layout.hero.y + layout.hero.height / 2, hit));
  assert(hit == 0);
  assert(fengyanRecentBookIndexFromPoint(
      recent, 3, 20, layout.mini[1].x + layout.mini[1].width / 2,
      layout.mini[1].y + layout.mini[1].height / 2, hit));
  assert(hit == 2);

  const Rect quick{0, 542, 480, 202};
  const auto quickLayout = makeFengyanMenuLayout(quick, 4, 20, 44, 4);
  assert(quickLayout.valid());
  assert(quickLayout.cols == 4);
  assert(quickLayout.rows == 1);
  for (int i = 0; i < 4; ++i) {
    const auto tile = quickLayout.tileRect(i);
    assert(tile.width == tile.height);
    assert(tile.y >= quick.y + 44);
  }

  std::puts("home reference geometry passed");
  return 0;
}

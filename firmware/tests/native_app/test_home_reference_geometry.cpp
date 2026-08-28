#include <assert.h>
#include <cstdio>

#include "util/TouchHitGeometry.h"

int main() {
  using namespace TouchHitGeometry;

  const Rect recent{0, 62, 480, 481};
  const auto layout = makeFengyanRecentLayout(recent, 3, 20);
  assert(layout.valid());
  assert(layout.bookCount == 3);
  assert(layout.heroCover.x == 33 && layout.heroCover.y == 105);
  assert(layout.heroCover.width == 158 && layout.heroCover.height == 222);
  assert(layout.miniCover[0].x == 40 && layout.miniCover[1].x == 161);
  assert(layout.miniCover[0].y == 363 && layout.miniCover[1].y == 363);
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

  const Rect quick{0, 557, 480, 172};
  const auto quickLayout = makeFengyanMenuLayout(quick, 4, 20, 46, 4);
  assert(quickLayout.valid());
  assert(quickLayout.cols == 4);
  assert(quickLayout.rows == 1);
  const int expectedX[4] = {33, 141, 248, 357};
  for (int i = 0; i < 4; ++i) {
    const auto tile = quickLayout.tileRect(i);
    assert(tile.width == 92);
    assert(tile.height == 107);
    assert(tile.x == expectedX[i]);
    assert(tile.y == 603);
  }

  std::puts("home reference geometry passed");
  return 0;
}

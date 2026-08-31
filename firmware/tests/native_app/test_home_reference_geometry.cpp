#include <assert.h>
#include <cstdio>

#include "util/TouchHitGeometry.h"

int main() {
  using namespace TouchHitGeometry;

  // Coordinates measured from the authored 480x800 transparent template and mapped to M4's 480x800 panel.
  const Rect recent{0, 77, 480, 543};
  const auto layout = makeFengyanRecentLayout(recent, 4, 20);
  assert(layout.valid());
  assert(layout.bookCount == 4);
  assert(layout.panel.x == 18 && layout.panel.y == 77);
  assert(layout.panel.width == 443 && layout.panel.height == 543);
  assert(layout.heroCover.x == 25 && layout.heroCover.y == 93);
  assert(layout.heroCover.width == 164 && layout.heroCover.height == 250);
  assert(layout.progress.x == 216 && layout.progress.y == 316);
  assert(layout.progress.width == 237 && layout.progress.height == 26);
  assert(layout.dividerY == 403);

  const int expectedMiniX[3] = {37, 185, 329};
  const int expectedMiniCenter[3] = {92, 238, 382};
  for (int i = 0; i < 3; ++i) {
    assert(layout.miniCover[i].x == expectedMiniX[i]);
    assert(layout.miniCover[i].y == 416);
    assert(layout.miniCover[i].width == (i == 0 ? 110 : 106));
    assert(layout.miniCover[i].height == 146);
    assert(layout.miniCover[i].x + layout.miniCover[i].width / 2 == expectedMiniCenter[i]);
  }
  assert(layout.hero.width > layout.mini[0].width);
  assert(layout.hero.height > layout.mini[0].height);
  assert(layout.hero.y < layout.mini[0].y);
  assert(layout.mini[0].y == layout.mini[1].y && layout.mini[1].y == layout.mini[2].y);

  int hit = -1;
  assert(fengyanRecentBookIndexFromPoint(
      recent, 4, 20, layout.hero.x + layout.hero.width / 2,
      layout.hero.y + layout.hero.height / 2, hit));
  assert(hit == 0);
  assert(fengyanRecentBookIndexFromPoint(
      recent, 4, 20, layout.mini[2].x + layout.mini[2].width / 2,
      layout.mini[2].y + layout.mini[2].height / 2, hit));
  assert(hit == 3);

  const Rect quick{0, 637, 480, 117};
  const auto quickLayout = makeFengyanMenuLayout(quick, 4, 20, 0, 4);
  assert(quickLayout.valid());
  assert(quickLayout.cols == 4);
  assert(quickLayout.rows == 1);
  const int expectedX[4] = {35, 141, 247, 357};
  for (int i = 0; i < 4; ++i) {
    const auto tile = quickLayout.tileRect(i);
    assert(tile.width == 86);
    assert(tile.height == 92);
    assert(tile.x == expectedX[i]);
    assert(tile.y == 650);
  }

  std::puts("home reference geometry passed");
  return 0;
}

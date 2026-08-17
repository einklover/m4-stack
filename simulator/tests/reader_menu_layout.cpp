#include "util/M4ReaderMenuLayout.h"

#include <cassert>
#include <iostream>

namespace {

bool overlaps(const TouchHitGeometry::Rect& a, const TouchHitGeometry::Rect& b) {
  return a.x < b.x + b.width && a.x + a.width > b.x &&
         a.y < b.y + b.height && a.y + a.height > b.y;
}

void assertInside(const TouchHitGeometry::Rect& r, int contentX, int contentWidth, int bottomLimit) {
  assert(r.width >= 48);
  assert(r.height >= 48);
  assert(r.x >= contentX);
  assert(r.x + r.width <= contentX + contentWidth);
  assert(r.y >= 0);
  assert(r.y + r.height <= bottomLimit);
}

void checkCase(const char* name, int contentX, int contentWidth, int top, int bottomLimit) {
  const auto quick = M4ReaderMenuLayout::makeQuickPanelLayout(contentX, contentWidth, top);
  assert(quick.progressBar.x >= contentX);
  assert(quick.progressBar.x + quick.progressBar.width <= contentX + contentWidth);
  assert(quick.progressBar.y >= 0);
  assert(quick.progressBar.y + quick.progressBar.height <= bottomLimit);
  assert(quick.indexFromPoint(quick.progressBar.x + quick.progressBar.width / 2,
                              quick.progressBar.y + quick.progressBar.height / 2) == -1);
  for (int i = 0; i < M4ReaderMenuLayout::kQuickActionCount; ++i) {
    const auto r = quick.actionRect(i);
    assertInside(r, contentX, contentWidth, bottomLimit);
    assert(quick.indexFromPoint(r.x + r.width / 2, r.y + r.height / 2) == i);
    for (int j = i + 1; j < M4ReaderMenuLayout::kQuickActionCount; ++j) {
      assert(!overlaps(r, quick.actionRect(j)));
    }
  }

  const auto style = M4ReaderMenuLayout::makeStylePanelLayout(contentX, contentWidth, top);
  assert(style.indexFromPoint(style.fontValue.x + style.fontValue.width / 2,
                              style.fontValue.y + style.fontValue.height / 2) == -1);
  for (int i = 0; i < M4ReaderMenuLayout::kStyleActionCount; ++i) {
    const auto r = style.actionRect(i);
    assertInside(r, contentX, contentWidth, bottomLimit);
    assert(style.indexFromPoint(r.x + r.width / 2, r.y + r.height / 2) == i);
    for (int j = i + 1; j < M4ReaderMenuLayout::kStyleActionCount; ++j) {
      assert(!overlaps(r, style.actionRect(j)));
    }
  }
  for (int i = 0; i < M4ReaderMenuLayout::kStyleActionCount; ++i) {
    assert(!overlaps(style.fontValue, style.actionRect(i)));
  }

  const auto progress = M4ReaderMenuLayout::makeProgressPanelLayout(contentX, contentWidth, top);
  assert(progress.value.x >= contentX);
  assert(progress.value.x + progress.value.width <= contentX + contentWidth);
  assert(progress.track.x >= contentX);
  assert(progress.track.x + progress.track.width <= contentX + contentWidth);
  assert(progress.percentFromPoint(progress.track.x,
                                   progress.track.y + progress.track.height / 2) == 0);
  assert(progress.percentFromPoint(progress.track.x + progress.track.width - 1,
                                   progress.track.y + progress.track.height / 2) == 100);
  const int middle = progress.percentFromPoint(progress.track.x + progress.track.width / 2,
                                                progress.track.y + progress.track.height / 2);
  assert(middle >= 49 && middle <= 51);
  assert(progress.percentFromPoint(progress.track.x - 1, progress.track.y) == -1);

  for (int i = 0; i < M4ReaderMenuLayout::kProgressStepCount; ++i) {
    const auto r = progress.stepRect(i);
    assertInside(r, contentX, contentWidth, bottomLimit);
    assert(progress.stepFromPoint(r.x + r.width / 2, r.y + r.height / 2) == i);
    for (int j = i + 1; j < M4ReaderMenuLayout::kProgressStepCount; ++j) {
      assert(!overlaps(r, progress.stepRect(j)));
    }
  }
  assert(progress.stepFromPoint(progress.track.x + progress.track.width / 2,
                                progress.track.y + progress.track.height / 2) == -1);

  std::cout << "reader-menu layout ok: " << name << "\n";
}

}  // namespace

int main() {
  // Murphy M4 logical dimensions and current Fengyan chrome metrics:
  // header=44, vertical spacing=16, button hints=40, landscape side gutter=30,
  // portrait-inverted top gutter=50.
  checkCase("portrait", 0, 480, 60, 744);
  checkCase("portrait-inverted", 0, 480, 110, 744);
  checkCase("landscape-cw", 30, 770, 60, 424);
  checkCase("landscape-ccw", 0, 770, 60, 424);
  return 0;
}

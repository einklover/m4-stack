#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

#include "util/M4FooterTouchPolicy.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4ProviderShelfIndex.h"
#include "util/M4WereadAuthPolicy.h"

int main() {
  // WeRead may report an expired login in an HTTP-200 JSON body rather than
  // only through HTTP 401/403. Entry validation must recognize both variants.
  assert(M4WereadAuthPolicy::responseIndicatesLoginRequired("{\"errCode\":-2012}"));
  assert(M4WereadAuthPolicy::responseIndicatesLoginRequired("{\"errMsg\":\"LOGIN_TIMEOUT\"}"));
  assert(!M4WereadAuthPolicy::responseIndicatesLoginRequired("{\"books\":[]}"));

  // 4096-row WeRead shelf: feed in deliberately awkward chunks so anchors do
  // not depend on line/chunk alignment.
  std::string shelf;
  shelf.reserve(4096 * 28);
  for (int i = 0; i < 4096; ++i) {
    shelf += "book" + std::to_string(i) + "\tTitle" + std::to_string(i) + "\tAuthor\t0\n";
  }
  M4ProviderShelfIndex::Builder idx;
  size_t off = 0;
  while (off < shelf.size()) {
    const size_t n = std::min<size_t>(37, shelf.size() - off);
    idx.feed(reinterpret_cast<const uint8_t*>(shelf.data() + off), n);
    off += n;
  }
  assert(idx.finish() == 4096);
  assert(idx.anchors.size() >= 256);
  assert(M4ProviderShelfIndex::anchorRow(0) == 0);
  assert(M4ProviderShelfIndex::anchorRow(17) == 16);
  assert(M4ProviderShelfIndex::anchorRow(4095) == 4080);

  // Native UI indexing is sliced and capped; malformed/unbounded shelf files
  // must stop without growing the anchor table forever.
  M4ProviderShelfIndex::Builder bounded;
  bounded.maxRows = 2;
  bounded.feed(reinterpret_cast<const uint8_t*>("a\nb\nc\n"), 6);
  assert(bounded.overflow);
  assert(bounded.finish() > bounded.maxRows);

  // NativeAppActivity uses the same page-step policy as the home shelf.
  constexpr int count = 4096;
  constexpr int pageItems = 6;
  int selected = 0;
  selected = M4ListTouchPolicy::applyPage(selected, count, pageItems, true);
  assert(selected == 6);
  selected = M4ListTouchPolicy::applyPage(selected, count, pageItems, true);
  assert(selected == 12);
  selected = 4086;
  selected = M4ListTouchPolicy::applyPage(selected, count, pageItems, true);
  assert(selected == 4092);
  selected = M4ListTouchPolicy::applyPage(selected, count, pageItems, true);
  assert(selected == 4095);
  selected = M4ListTouchPolicy::applyPage(selected, count, pageItems, false);
  assert(selected == 4089);

  // A tap on page N must resolve to the absolute shelf index, not page-local 0..N.
  int hit = -1;
  assert(TouchHitGeometry::listIndexFromPoint(120, 100, 360, 60, count, 3000, hit));
  assert(hit == 3000);
  assert(TouchHitGeometry::listIndexFromPoint(299, 100, 360, 60, count, 3000, hit));
  assert(hit == 3003);

  // A held modal tap can report both down and release; release must activate
  // the dialog button instead of being consumed as selection-only.
  const auto dialog = M4ListTouchPolicy::makeCenteredTwoButtons(480, 456, 60, 28, 30, 2);
  M4ListTouchPolicy::Event dialogTap{};
  dialogTap.touchDown = true;
  dialogTap.tap = true;
  dialogTap.x = dialog.buttonRect(1).x + 30;
  dialogTap.y = dialog.buttonRect(1).y + 14;
  assert(M4ListTouchPolicy::resolveDialog(dialogTap, dialog, hit) == M4ListTouchPolicy::Action::DialogPick);
  assert(hit == 1);

  // Fullscreen provider footer hit geometry: four physical slots across 480px.
  assert(M4FooterTouchPolicy::slotFromPoint(10, 770, 480, 800, 46) == 0);
  assert(M4FooterTouchPolicy::slotFromPoint(130, 770, 480, 800, 46) == 1);
  assert(M4FooterTouchPolicy::slotFromPoint(260, 770, 480, 800, 46) == 2);
  assert(M4FooterTouchPolicy::slotFromPoint(470, 770, 480, 800, 46) == 3);
  assert(M4FooterTouchPolicy::slotFromPoint(100, 740, 480, 800, 46) == -1);

  return 0;
}

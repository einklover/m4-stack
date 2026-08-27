#include <cassert>

#include "apps/native/M4NativeUi.h"
#include "util/M4FooterTouchPolicy.h"
#include "util/M4ListTouchPolicy.h"
#include "util/TouchHitGeometry.h"

int main() {
  int hit = -1;
  M4ListTouchPolicy::ListLayout list;
  list.listTop = 100;
  list.listHeight = 360;
  list.rowStep = 60;
  list.itemCount = 8;
  list.selectedIndex = 0;
  const auto frame = M4ListTouchPolicy::mergeFrame(
      false, M4ListTouchPolicy::Swipe::None, true, 10, 130, true, 10, 130);
  assert(M4ListTouchPolicy::resolveList(frame, list, hit) == M4ListTouchPolicy::Action::Activate);
  assert(hit == 0);

  const auto lyraMenu = TouchHitGeometry::makeLyraMenuLayout({0, 351, 480, 401}, 6);
  assert(TouchHitGeometry::lyraMenuIndexFromPoint({0, 351, 480, 401}, 6, 30, 500, hit));
  assert(hit == 4);
  assert(TouchHitGeometry::lyraMenuIndexFromPoint({0, 351, 480, 401}, 6, 250, 500, hit));
  assert(hit == 5);
  assert(!TouchHitGeometry::lyraMenuIndexFromPoint({0, 351, 480, 401}, 6, 30, 760, hit));
  (void)lyraMenu;

  assert(M4FooterTouchPolicy::slotFromPoint(40, 760, 480, 800, 40) == 0);
  assert(M4FooterTouchPolicy::slotFromPoint(160, 760, 480, 800, 40) == 1);
  assert(M4FooterTouchPolicy::slotFromPoint(275, 760, 480, 800, 40) == 2);
  assert(M4FooterTouchPolicy::slotFromPoint(400, 760, 480, 800, 40) == 3);
  assert(M4FooterTouchPolicy::slotFromPoint(145, 760, 480, 800, 40) == -1);

  const auto footer = M4NativeUi::ProviderFooterLayout::make(
      480, 800, (const bool[4]){true, false, true, false});
  assert(footer.buttonAt(240, 760) == -1);
  assert(footer.buttonAt(100, 760) == 0);
  assert(footer.buttonAt(360, 760) == 2);
  return 0;
}

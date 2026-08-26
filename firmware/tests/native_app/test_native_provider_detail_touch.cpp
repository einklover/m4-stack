#include <cassert>
#include <cstdio>

#include "util/M4NativeProviderDetailTouchPolicy.h"

int main() {
  using namespace M4NativeProviderDetailTouchPolicy;

  Layout layout;
  layout.reset(480, 800, 46);
  layout.setReadButton(100, 48);
  layout.setChapterBlock(180, 360);

  assert(layout.actionAt(10, 120) == Action::Read);
  assert(layout.actionAt(10, 180) == Action::Chapter);
  assert(layout.actionAt(479, 359) == Action::Chapter);
  assert(layout.actionAt(10, 760) == Action::Back);
  assert(layout.actionAt(470, 799) == Action::Back);
  assert(layout.actionAt(10, 170) == Action::None);

  // A rerender that does not paint the chapter block cannot retain the old
  // hitbox; the read button and full footer remain independent targets.
  layout.reset(480, 800, 46);
  layout.setReadButton(100, 48);
  assert(layout.actionAt(10, 180) == Action::None);
  assert(layout.actionAt(10, 760) == Action::Back);

  std::puts("native provider detail touch geometry: PASS");
  return 0;
}

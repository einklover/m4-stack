#pragma once

// Pure list / dialog touch interaction policy (host-testable).
// Activities map MappedInputManager events into these helpers; geometry lives in TouchHitGeometry.

#include <cstdint>

#include "TouchHitGeometry.h"

namespace M4ListTouchPolicy {

enum class Swipe : uint8_t { None, Up, Down, Left, Right };

enum class Action : uint8_t {
  None = 0,
  Select,     // highlight row only
  Activate,   // open / confirm selection
  PageUp,     // swipe scroll
  PageDown,
  Back,       // edge back / cancel
  DialogPick  // dialog button index in outIndex
};

struct ListLayout {
  int listTop = 0;
  int listHeight = 0;
  int rowStep = 40;
  int itemCount = 0;
  int selectedIndex = 0;
  // Scroll-window mode (Wi-Fi style): fixed maxVisible + selected-driven offset.
  // If maxVisible > 0, use scrollOffsetFromSelected; else paged listIndexFromPoint.
  int maxVisible = 0;
};

inline int scrollOffsetFromSelected(int selectedIndex, int itemCount, int maxVisible) {
  if (maxVisible <= 0 || itemCount <= 0) return 0;
  if (selectedIndex < 0) selectedIndex = 0;
  if (selectedIndex >= itemCount) selectedIndex = itemCount - 1;
  if (selectedIndex >= maxVisible) return selectedIndex - maxVisible + 1;
  return 0;
}

// Wi-Fi style fixed line list: index from point with scroll window.
inline bool scrollListIndexFromPoint(int y, int listTop, int rowStep, int maxVisible, int itemCount,
                                     int selectedIndex, int& outIndex) {
  if (itemCount <= 0 || rowStep <= 0 || maxVisible <= 0) return false;
  if (y < listTop) return false;
  const int row = (y - listTop) / rowStep;
  if (row < 0 || row >= maxVisible) return false;
  const int scroll = scrollOffsetFromSelected(selectedIndex, itemCount, maxVisible);
  const int idx = scroll + row;
  if (idx < 0 || idx >= itemCount) return false;
  outIndex = idx;
  return true;
}

struct Event {
  bool backGesture = false;
  Swipe swipe = Swipe::None;
  bool touchDown = false;
  bool tap = false;
  int x = 0;
  int y = 0;
  // When both down and tap are true in one sample, process order is:
  // back → swipe → tap(activate) → touchDown(select). Never use else-if that
  // drops tap when down is also set.
};

// Merge independently sampled flags (do not else-if away tap when down is set).
inline Event mergeFrame(bool backGesture, Swipe swipe, bool touchDown, int dx, int dy, bool tap, int tx, int ty) {
  Event e;
  e.backGesture = backGesture;
  e.swipe = swipe;
  e.touchDown = touchDown;
  e.tap = tap;
  // Prefer tap coordinates for activate; else down.
  if (tap) {
    e.x = tx;
    e.y = ty;
  } else if (touchDown) {
    e.x = dx;
    e.y = dy;
  }
  return e;
}

// Order: back → swipe (no activate) → tap activate → down select.
// If both tap and down are set (same frame), prefer Activate once (no double fire).
inline Action resolveList(const Event& e, const ListLayout& L, int& outIndex) {
  outIndex = -1;
  if (e.backGesture) return Action::Back;

  if (e.swipe == Swipe::Up || e.swipe == Swipe::Left) return Action::PageDown;
  if (e.swipe == Swipe::Down || e.swipe == Swipe::Right) return Action::PageUp;

  auto hit = [&](int& idx) -> bool {
    if (L.maxVisible > 0) {
      return scrollListIndexFromPoint(e.y, L.listTop, L.rowStep, L.maxVisible, L.itemCount, L.selectedIndex,
                                      idx);
    }
    return TouchHitGeometry::listIndexFromPoint(e.y, L.listTop, L.listHeight, L.rowStep, L.itemCount,
                                                L.selectedIndex, idx);
  };

  if (e.tap) {
    int idx = -1;
    if (hit(idx)) {
      outIndex = idx;
      return Action::Activate;
    }
    // Tap missed list: do not also select via down in same event
    return Action::None;
  }
  if (e.touchDown) {
    int idx = -1;
    if (hit(idx)) {
      outIndex = idx;
      return Action::Select;
    }
    return Action::None;
  }
  return Action::None;
}

// Horizontal two-button dialog (Yes/No), matching WifiSelection save/forget prompts.
struct DialogTwoButtonLayout {
  int buttonY = 0;
  int buttonWidth = 60;
  int buttonHeight = 28;
  int buttonSpacing = 30;
  int startX = 0;
  int count = 2;

  TouchHitGeometry::Rect buttonRect(int index) const {
    if (index < 0 || index >= count) return {};
    return {startX + index * (buttonWidth + buttonSpacing), buttonY, buttonWidth, buttonHeight};
  }
};

inline DialogTwoButtonLayout makeCenteredTwoButtons(int screenW, int buttonY, int buttonWidth = 60,
                                                    int buttonHeight = 28, int buttonSpacing = 30,
                                                    int count = 2) {
  DialogTwoButtonLayout L;
  L.buttonY = buttonY;
  L.buttonWidth = buttonWidth;
  L.buttonHeight = buttonHeight;
  L.buttonSpacing = buttonSpacing;
  L.count = count > 0 ? count : 2;
  const int totalW = L.buttonWidth * L.count + L.buttonSpacing * (L.count - 1);
  L.startX = (screenW - totalW) / 2;
  return L;
}

inline bool dialogButtonFromPoint(const DialogTwoButtonLayout& L, int px, int py, int& outIndex) {
  for (int i = 0; i < L.count; ++i) {
    if (L.buttonRect(i).contains(px, py)) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

inline Action resolveDialog(const Event& e, const DialogTwoButtonLayout& L, int& outIndex) {
  outIndex = -1;
  if (e.backGesture) return Action::Back;
  // A release can report both touchDown and tap after the hold threshold.
  // The tap is the activating edge; do not downgrade it to selection.
  if (e.tap) {
    int idx = -1;
    if (dialogButtonFromPoint(L, e.x, e.y, idx)) {
      outIndex = idx;
      return Action::DialogPick;
    }
    return Action::None;
  }
  if (e.touchDown) {
    int idx = -1;
    if (dialogButtonFromPoint(L, e.x, e.y, idx)) {
      outIndex = idx;
      return Action::Select;
    }
    return Action::None;
  }
  return Action::None;
}

// Apply page step to selection (clamp).
inline int applyPage(int selected, int itemCount, int pageItems, bool pageDown) {
  if (itemCount <= 0) return 0;
  const int step = pageItems > 0 ? pageItems : 1;
  if (pageDown) {
    int n = selected + step;
    if (n >= itemCount) n = itemCount - 1;
    return n < 0 ? 0 : n;
  }
  int n = selected - step;
  return n < 0 ? 0 : n;
}

}  // namespace M4ListTouchPolicy

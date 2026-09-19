#pragma once

// 8 px focus bar, dirty union of old∪new row, burst-6 then full. Zero animation.

struct M4FocusRect {
  int x;
  int y;
  int w;
  int h;
};

struct M4DirtyUnion {
  int y0;
  int y1;
  bool full;
};

constexpr int kM4PartialBurst = 6;

inline M4FocusRect m4SettingsFocusBarLocal() {
  return M4FocusRect{0, 12, 8, 56};
}

inline M4DirtyUnion m4SettingsDirtyAfterMove(int oldSlot, int newSlot, int itemH, int gap, int burstCount) {
  M4DirtyUnion out{};
  if (burstCount >= kM4PartialBurst) {
    out.full = true;
    out.y0 = 0;
    out.y1 = 0;
    return out;
  }
  if (oldSlot > newSlot) {
    const int tmp = oldSlot;
    oldSlot = newSlot;
    newSlot = tmp;
  }
  if (oldSlot < 0) oldSlot = 0;
  if (newSlot < oldSlot) newSlot = oldSlot;
  const int stride = itemH + gap;
  out.y0 = oldSlot * stride;
  out.y1 = newSlot * stride + itemH;
  out.full = false;
  return out;
}

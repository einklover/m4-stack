#pragma once

#include <cstdint>

namespace M4FooterTouchPolicy {

enum LogicalButton : uint8_t {
  Back = 1u << 0,
  Confirm = 1u << 1,
  Left = 1u << 2,
  Right = 1u << 3,
};

inline uint8_t& maskStorage() {
  static uint8_t mask = 0;
  return mask;
}

inline void setMask(uint8_t mask) { maskStorage() = mask; }
inline uint8_t mask() { return maskStorage(); }
inline bool enabled(LogicalButton button) { return (maskStorage() & static_cast<uint8_t>(button)) != 0; }

// Returns the physical four-slot footer index (0..3), or -1 when the point is
// outside one of the painted bottom hint buttons. Keep these rectangles in
// lockstep with FengyanTheme/LyraTheme::drawButtonHints.
inline int slotFromPoint(int x, int y, int screenWidth, int screenHeight, int footerHeight) {
  if (screenWidth <= 0 || screenHeight <= 0 || footerHeight <= 0) return -1;
  if (x < 0 || x >= screenWidth || y < screenHeight - footerHeight || y >= screenHeight) return -1;
  constexpr int buttonWidth = 106;
  constexpr int buttonPositions[] = {38, 154, 268, 384};
  for (int slot = 0; slot < 4; ++slot) {
    if (x >= buttonPositions[slot] && x < buttonPositions[slot] + buttonWidth) return slot;
  }
  return -1;
}

}  // namespace M4FooterTouchPolicy

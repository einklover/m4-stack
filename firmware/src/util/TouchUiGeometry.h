#pragma once

// Pure touch UI geometry shared by drawing and hit testing. Keep this file
// hardware-free so the 480x800 M4 touch contracts can run on the host.

#include "TouchHitGeometry.h"

namespace TouchHitGeometry {

enum class TouchKeyboardMode { Letters, Symbols };
enum class TouchKeyboardKeyKind { Character, Mode, Shift, Space, Backspace, Confirm };

struct TouchKeyboardState {
  TouchKeyboardMode mode = TouchKeyboardMode::Letters;
  bool shifted = false;

  void toggleShift() {
    if (mode == TouchKeyboardMode::Letters) shifted = !shifted;
  }

  void toggleMode() {
    mode = mode == TouchKeyboardMode::Letters ? TouchKeyboardMode::Symbols : TouchKeyboardMode::Letters;
    shifted = false;
  }

  char resolveCharacter(char c) {
    if (mode == TouchKeyboardMode::Letters && shifted && c >= 'a' && c <= 'z') {
      shifted = false;
      return static_cast<char>(c - 'a' + 'A');
    }
    if (mode == TouchKeyboardMode::Letters) shifted = false;
    return c;
  }
};

struct TouchKeyboardKey {
  TouchKeyboardKeyKind kind = TouchKeyboardKeyKind::Character;
  char character = '\0';
  Rect rect{};
};

struct TouchKeyboardLayout {
  static constexpr int kMaxKeys = 48;
  TouchKeyboardKey keys[kMaxKeys]{};
  int keyCount = 0;
  int characterRowCount = 0;
  int characterCounts[4] = {0, 0, 0, 0};
  Rect characterRowBounds[4]{};
  int maxCharactersPerRow = 0;
  int keyHeight = 0;
  int keySpacing = 0;

  bool valid() const { return keyCount > 0 && characterRowCount >= 3 && keyHeight > 0; }

  bool hit(int px, int py, int& outIndex) const {
    for (int i = 0; i < keyCount; ++i) {
      if (keys[i].rect.contains(px, py)) {
        outIndex = i;
        return true;
      }
    }
    return false;
  }

  const TouchKeyboardKey* find(TouchKeyboardKeyKind kind) const {
    for (int i = 0; i < keyCount; ++i) {
      if (keys[i].kind == kind) return &keys[i];
    }
    return nullptr;
  }

  bool containsCharacter(char c) const {
    for (int i = 0; i < keyCount; ++i) {
      if (keys[i].kind == TouchKeyboardKeyKind::Character && keys[i].character == c) return true;
    }
    return false;
  }
};

inline TouchKeyboardLayout makeTouchKeyboardLayout(int pageWidth, int keyboardStartY, int keyHeight,
                                                   int keySpacing, TouchKeyboardMode mode,
                                                   int sideMargin = 8) {
  TouchKeyboardLayout L;
  L.keyHeight = keyHeight;
  L.keySpacing = keySpacing;
  if (pageWidth <= 0 || keyHeight <= 0 || keySpacing < 0 || sideMargin < 0) return L;

  const char* rows[4] = {};
  int rowCount = 0;
  if (mode == TouchKeyboardMode::Letters) {
    rows[0] = "qwertyuiop";
    rows[1] = "asdfghjkl";
    rows[2] = "zxcvbnm";
    rowCount = 3;
  } else {
    rows[0] = "1234567890";
    rows[1] = "-_=+[]{}()";
    rows[2] = "@#$%&*!?/";
    rows[3] = ".,:;'\"\\`~";
    rowCount = 4;
  }

  L.characterRowCount = rowCount;
  int maxCount = 0;
  for (int row = 0; row < rowCount; ++row) {
    int count = 0;
    while (rows[row][count] != '\0') ++count;
    L.characterCounts[row] = count;
    maxCount = maxInt(maxCount, count);
  }
  L.maxCharactersPerRow = maxCount;

  const int usableWidth = pageWidth - sideMargin * 2;
  if (usableWidth <= 0 || maxCount <= 0) return L;
  const int keyWidth = (usableWidth - (maxCount - 1) * keySpacing) / maxCount;
  if (keyWidth <= 0) return L;

  for (int row = 0; row < rowCount; ++row) {
    const int count = L.characterCounts[row];
    const int rowWidth = count * keyWidth + (count - 1) * keySpacing;
    const int startX = (pageWidth - rowWidth) / 2;
    const int y = keyboardStartY + row * (keyHeight + keySpacing);
    L.characterRowBounds[row] = {startX, y, rowWidth, keyHeight};
    for (int col = 0; col < count && L.keyCount < TouchKeyboardLayout::kMaxKeys; ++col) {
      L.keys[L.keyCount++] = {TouchKeyboardKeyKind::Character, rows[row][col],
                              {startX + col * (keyWidth + keySpacing), y, keyWidth, keyHeight}};
    }
  }

  const int controlY = keyboardStartY + rowCount * (keyHeight + keySpacing);
  const int x0 = sideMargin;
  const int controlUsable = pageWidth - sideMargin * 2;

  auto addControl = [&](TouchKeyboardKeyKind kind, int x, int width) {
    if (L.keyCount < TouchKeyboardLayout::kMaxKeys && width > 0) {
      L.keys[L.keyCount++] = {kind, '\0', {x, controlY, width, keyHeight}};
    }
  };

  if (mode == TouchKeyboardMode::Letters) {
    // 480px target: 70 + 70 + 150 + 70 + 80 = 440px plus four 6px gaps.
    const int gaps = keySpacing * 4;
    const int available = controlUsable - gaps;
    const int modeW = maxInt(64, available * 7 / 44);
    const int shiftW = maxInt(64, available * 7 / 44);
    const int backW = maxInt(64, available * 7 / 44);
    const int confirmW = maxInt(72, available * 8 / 44);
    const int spaceW = available - modeW - shiftW - backW - confirmW;
    int x = x0;
    addControl(TouchKeyboardKeyKind::Mode, x, modeW);
    x += modeW + keySpacing;
    addControl(TouchKeyboardKeyKind::Shift, x, shiftW);
    x += shiftW + keySpacing;
    addControl(TouchKeyboardKeyKind::Space, x, spaceW);
    x += spaceW + keySpacing;
    addControl(TouchKeyboardKeyKind::Backspace, x, backW);
    x += backW + keySpacing;
    addControl(TouchKeyboardKeyKind::Confirm, x, confirmW);
  } else {
    const int gaps = keySpacing * 3;
    const int available = controlUsable - gaps;
    const int modeW = maxInt(74, available * 8 / 44);
    const int backW = maxInt(74, available * 8 / 44);
    const int confirmW = maxInt(84, available * 9 / 44);
    const int spaceW = available - modeW - backW - confirmW;
    int x = x0;
    addControl(TouchKeyboardKeyKind::Mode, x, modeW);
    x += modeW + keySpacing;
    addControl(TouchKeyboardKeyKind::Space, x, spaceW);
    x += spaceW + keySpacing;
    addControl(TouchKeyboardKeyKind::Backspace, x, backW);
    x += backW + keySpacing;
    addControl(TouchKeyboardKeyKind::Confirm, x, confirmW);
  }

  return L;
}

struct WifiNetworkListLayout {
  int screenWidth = 0;
  int screenHeight = 0;
  int rowTop = 0;
  int rowHeight = 68;
  int rowGap = 6;
  int rowWidth = 0;
  int sideMargin = 12;
  int visibleRows = 0;
  Rect refresh{};

  bool valid() const {
    return screenWidth > 0 && screenHeight > 0 && rowWidth > 0 && rowHeight >= 1 && visibleRows > 0 &&
           refresh.width > 0 && refresh.height > 0;
  }

  Rect rowRect(int visibleIndex) const {
    if (visibleIndex < 0 || visibleIndex >= visibleRows) return {};
    return {sideMargin, rowTop + visibleIndex * (rowHeight + rowGap), rowWidth, rowHeight};
  }

  bool hitRow(int px, int py, int itemCount, int& outIndex) const {
    const int count = minInt(maxInt(itemCount, 0), visibleRows);
    for (int i = 0; i < count; ++i) {
      if (rowRect(i).contains(px, py)) {
        outIndex = i;
        return true;
      }
    }
    return false;
  }
};

inline WifiNetworkListLayout makeWifiNetworkListLayout(int screenWidth, int screenHeight, int itemCount,
                                                        int rowTop = 92, int rowHeight = 68, int rowGap = 6,
                                                        int sideMargin = 12) {
  WifiNetworkListLayout L;
  L.screenWidth = screenWidth;
  L.screenHeight = screenHeight;
  L.rowTop = rowTop;
  L.rowHeight = rowHeight;
  L.rowGap = rowGap;
  L.sideMargin = sideMargin;
  if (screenWidth <= sideMargin * 2 || screenHeight <= rowTop || rowHeight <= 0) return L;
  L.rowWidth = screenWidth - sideMargin * 2;
  const int bottomReserve = 20;
  L.visibleRows = maxInt(1, (screenHeight - rowTop - bottomReserve + rowGap) / (rowHeight + rowGap));
  if (itemCount > 0) L.visibleRows = minInt(L.visibleRows, itemCount);
  const int refreshW = 104;
  const int refreshH = 56;
  L.refresh = {screenWidth - sideMargin - refreshW, 18, refreshW, refreshH};
  return L;
}

}  // namespace TouchHitGeometry

#pragma once

#include "TouchHitGeometry.h"

#include <algorithm>

// Pure, host-testable geometry shared by reader-menu drawing and touch hit-testing.
// Keep this file free of Arduino/GfxRenderer dependencies so simulator tests can
// validate every M4 orientation without booting firmware.
namespace M4ReaderMenuLayout {

constexpr int kQuickProgressBarHeight = 8;
constexpr int kQuickProgressBlockHeight = 32;
constexpr int kQuickActionCount = 6;
constexpr int kStyleActionCount = 7;

struct QuickPanelLayout {
  TouchHitGeometry::Rect progressBar{};
  TouchHitGeometry::Rect actions[kQuickActionCount]{};

  TouchHitGeometry::Rect actionRect(int index) const {
    return (index >= 0 && index < kQuickActionCount) ? actions[index] : TouchHitGeometry::Rect{};
  }

  int indexFromPoint(int x, int y) const {
    for (int i = 0; i < kQuickActionCount; ++i) {
      if (actions[i].contains(x, y)) return i;
    }
    return -1;
  }
};

inline QuickPanelLayout makeQuickPanelLayout(int contentX, int contentWidth, int top) {
  QuickPanelLayout L;
  constexpr int side = 20;
  constexpr int colGap = 12;
  constexpr int rowGap = 12;
  constexpr int chipH = 72;

  const int innerX = contentX + side;
  const int innerW = std::max(120, contentWidth - side * 2);
  const int chipW = std::max(36, (innerW - colGap * 2) / 3);
  const int usedW = chipW * 3 + colGap * 2;
  const int rowX = innerX + std::max(0, (innerW - usedW) / 2);

  L.progressBar = {innerX, top, innerW, kQuickProgressBarHeight};
  const int gridY = top + kQuickProgressBlockHeight;
  for (int i = 0; i < kQuickActionCount; ++i) {
    const int row = i / 3;
    const int col = i % 3;
    L.actions[i] = {rowX + col * (chipW + colGap), gridY + row * (chipH + rowGap), chipW, chipH};
  }
  return L;
}

struct StylePanelLayout {
  int labelX = 0;
  int fontLabelY = 0;
  int layoutLabelY = 0;
  TouchHitGeometry::Rect fontMinus{};
  TouchHitGeometry::Rect fontValue{};  // display-only: deliberately excluded from hit-testing
  TouchHitGeometry::Rect fontPlus{};
  TouchHitGeometry::Rect compact{};
  TouchHitGeometry::Rect standard{};
  TouchHitGeometry::Rect relaxed{};
  TouchHitGeometry::Rect fontPicker{};
  TouchHitGeometry::Rect details{};

  TouchHitGeometry::Rect actionRect(int index) const {
    switch (index) {
      case 0: return fontMinus;
      case 1: return fontPlus;
      case 2: return compact;
      case 3: return standard;
      case 4: return relaxed;
      case 5: return fontPicker;
      case 6: return details;
      default: return {};
    }
  }

  int indexFromPoint(int x, int y) const {
    for (int i = 0; i < kStyleActionCount; ++i) {
      if (actionRect(i).contains(x, y)) return i;
    }
    return -1;
  }
};

inline StylePanelLayout makeStylePanelLayout(int contentX, int contentWidth, int top) {
  StylePanelLayout L;
  constexpr int side = 20;
  constexpr int gap = 12;
  constexpr int labelH = 24;
  constexpr int chipH = 54;
  constexpr int sectionGap = 12;
  constexpr int bottomGap = 18;

  const int innerX = contentX + side;
  const int innerW = std::max(120, contentWidth - side * 2);
  const int chipW = std::max(36, (innerW - gap * 2) / 3);
  const int thirdUsed = chipW * 3 + gap * 2;
  const int rowX = innerX + std::max(0, (innerW - thirdUsed) / 2);

  L.labelX = innerX;
  L.fontLabelY = top;
  const int fontY = top + labelH;
  L.fontMinus = {rowX, fontY, chipW, chipH};
  L.fontValue = {rowX + chipW + gap, fontY, chipW, chipH};
  L.fontPlus = {rowX + (chipW + gap) * 2, fontY, chipW, chipH};

  L.layoutLabelY = fontY + chipH + sectionGap;
  const int layoutY = L.layoutLabelY + labelH;
  L.compact = {rowX, layoutY, chipW, chipH};
  L.standard = {rowX + chipW + gap, layoutY, chipW, chipH};
  L.relaxed = {rowX + (chipW + gap) * 2, layoutY, chipW, chipH};

  const int bottomY = layoutY + chipH + bottomGap;
  const int bottomW = std::max(50, (innerW - gap) / 2);
  L.fontPicker = {innerX, bottomY, bottomW, chipH};
  L.details = {innerX + bottomW + gap, bottomY, bottomW, chipH};
  return L;
}

}  // namespace M4ReaderMenuLayout

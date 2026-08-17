#pragma once

#include "TouchHitGeometry.h"

#include <algorithm>

// Pure, host-testable geometry shared by reader-menu drawing and touch hit-testing.
// Keep this file free of Arduino/GfxRenderer dependencies so simulator tests can
// validate every M4 orientation without booting firmware.
namespace M4ReaderMenuLayout {

constexpr int kQuickProgressBarHeight = 8;
// Reserve enough vertical space for BaseTheme's percentage label below the
// compact bar, then a clean gap before the 3x2 action grid. The progress readout
// is useful information; it should look like a distinct info band, not part of
// the Progress action tile.
constexpr int kQuickProgressBlockHeight = 52;
constexpr int kQuickActionCount = 6;
constexpr int kStyleActionCount = 7;
constexpr int kProgressStepCount = 4;

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
  constexpr int chipH = 88;

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

  TouchHitGeometry::Rect fontGroupRect() const {
    return {fontMinus.x, fontMinus.y,
            fontPlus.x + fontPlus.width - fontMinus.x, fontMinus.height};
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
  constexpr int fontGroupH = 58;
  constexpr int chipH = 54;
  constexpr int sectionGap = 14;
  constexpr int bottomGap = 18;

  const int innerX = contentX + side;
  const int innerW = std::max(120, contentWidth - side * 2);

  L.labelX = innerX;
  L.fontLabelY = top;
  const int fontY = top + labelH;
  // One connected three-cell group mirrors touch-reader typography controls:
  // A- | current size | A+. The center cell is intentionally display-only.
  const int fontCellW = std::max(36, innerW / 3);
  const int fontRemainder = innerW - fontCellW * 2;
  L.fontMinus = {innerX, fontY, fontCellW, fontGroupH};
  L.fontValue = {innerX + fontCellW, fontY, fontRemainder, fontGroupH};
  L.fontPlus = {innerX + fontCellW + fontRemainder, fontY, fontCellW, fontGroupH};

  L.layoutLabelY = fontY + fontGroupH + sectionGap;
  const int layoutY = L.layoutLabelY + labelH;
  const int chipW = std::max(36, (innerW - gap * 2) / 3);
  const int thirdUsed = chipW * 3 + gap * 2;
  const int rowX = innerX + std::max(0, (innerW - thirdUsed) / 2);
  L.compact = {rowX, layoutY, chipW, chipH};
  L.standard = {rowX + chipW + gap, layoutY, chipW, chipH};
  L.relaxed = {rowX + (chipW + gap) * 2, layoutY, chipW, chipH};

  const int bottomY = layoutY + chipH + bottomGap;
  const int bottomW = std::max(50, (innerW - gap) / 2);
  L.fontPicker = {innerX, bottomY, bottomW, chipH};
  L.details = {innerX + bottomW + gap, bottomY, bottomW, chipH};
  return L;
}

// E-ink progress selector. The track is tappable but deliberately not draggable:
// one gesture -> one state change -> one refresh. Four discrete step chips cover
// coarse/fine navigation without requiring a precision slider.
struct ProgressPanelLayout {
  TouchHitGeometry::Rect value{};
  TouchHitGeometry::Rect track{};
  TouchHitGeometry::Rect trackHit{};
  TouchHitGeometry::Rect steps[kProgressStepCount]{};

  TouchHitGeometry::Rect stepRect(int index) const {
    return (index >= 0 && index < kProgressStepCount) ? steps[index] : TouchHitGeometry::Rect{};
  }

  int stepFromPoint(int x, int y) const {
    for (int i = 0; i < kProgressStepCount; ++i) {
      if (steps[i].contains(x, y)) return i;
    }
    return -1;
  }

  int percentFromPoint(int x, int y) const {
    if (!trackHit.contains(x, y) || track.width <= 1) return -1;
    const int clampedX = std::max(track.x, std::min(track.x + track.width - 1, x));
    return ((clampedX - track.x) * 100 + (track.width - 1) / 2) / (track.width - 1);
  }
};

inline ProgressPanelLayout makeProgressPanelLayout(int contentX, int contentWidth, int top) {
  ProgressPanelLayout L;
  constexpr int side = 24;
  constexpr int valueH = 58;
  constexpr int trackGap = 16;
  constexpr int trackH = 10;
  constexpr int trackTouchPad = 16;
  constexpr int stepGapY = 34;
  constexpr int stepGapX = 10;
  constexpr int stepH = 56;

  const int innerX = contentX + side;
  const int innerW = std::max(160, contentWidth - side * 2);
  L.value = {innerX, top, innerW, valueH};

  const int trackY = top + valueH + trackGap;
  L.track = {innerX, trackY, innerW, trackH};
  L.trackHit = {innerX, trackY - trackTouchPad, innerW, trackH + trackTouchPad * 2};

  const int stepY = trackY + trackH + stepGapY;
  const int stepW = std::max(48, (innerW - stepGapX * (kProgressStepCount - 1)) / kProgressStepCount);
  const int usedW = stepW * kProgressStepCount + stepGapX * (kProgressStepCount - 1);
  const int startX = innerX + std::max(0, (innerW - usedW) / 2);
  for (int i = 0; i < kProgressStepCount; ++i) {
    L.steps[i] = {startX + i * (stepW + stepGapX), stepY, stepW, stepH};
  }
  return L;
}

}  // namespace M4ReaderMenuLayout

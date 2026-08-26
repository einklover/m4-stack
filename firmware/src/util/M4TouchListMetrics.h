#pragma once

#include <algorithm>

#include "TouchHitGeometry.h"

// Touch-friendly list/TOC metrics for M4 (host-testable pure helpers).
// Physical-button builds keep denser rows; touch targets aim ~48–56px.

namespace M4TouchListMetrics {

constexpr int kChapterBackHitWidth = 56;
constexpr int kChapterBackHitHeight = 56;
constexpr int kChapterBackTitleGap = 8;

inline int chapterFooterReserve(bool touch);

struct ChapterListLayout {
  TouchHitGeometry::Rect backVisual;
  TouchHitGeometry::Rect backHitbox;
  TouchHitGeometry::Rect header;
  TouchHitGeometry::Rect headerText;
  TouchHitGeometry::Rect list;
  TouchHitGeometry::Rect firstRow;
  TouchHitGeometry::Rect viewport;
  TouchHitGeometry::Rect footer;
  int rowHeight = 0;
  int systemLineHeight = 0;
  int headerHeight = 0;

  bool valid() const { return list.width > 0 && list.height >= 0 && rowHeight > 0; }
};

inline bool disjoint(const TouchHitGeometry::Rect& a, const TouchHitGeometry::Rect& b) {
  return a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y ||
         b.y + b.height <= a.y;
}

// Build the complete chapter-picker frame from the measured system UI face.
// `systemLineHeight` is supplied by M4UiText::systemListLineHeight(), so the
// row/header model follows the actual active small/medium/large system tier.
inline ChapterListLayout makeChapterListLayout(int screenWidth, int screenHeight, bool touch,
                                               TouchHitGeometry::Orientation orientation,
                                               int systemLineHeight, int topPadding = 5,
                                               int themeHeaderHeight = 44, int verticalSpacing = 16) {
  ChapterListLayout layout;
  layout.systemLineHeight = std::max(1, systemLineHeight);
  layout.headerHeight = std::max(themeHeaderHeight, layout.systemLineHeight + 12);
  layout.rowHeight = std::max(touch ? 52 : 30, layout.systemLineHeight + (touch ? 20 : 8));

  const bool landscapeCw = orientation == TouchHitGeometry::Orientation::LandscapeClockwise;
  const bool invertedPortrait = orientation == TouchHitGeometry::Orientation::PortraitInverted;
  const int contentX = landscapeCw ? 30 : 0;
  const int contentY = invertedPortrait ? 50 : 0;
  const int contentWidth = std::max(0, screenWidth - contentX);
  const int headerTop = contentY + topPadding;
  const int listTop = headerTop + layout.headerHeight + verticalSpacing;
  const int footerReserve = chapterFooterReserve(touch);
  const int footerTop = std::max(listTop, screenHeight - footerReserve);
  const int listHeight = std::max(0, footerTop - listTop);

  layout.backHitbox = {0, 0, std::min(kChapterBackHitWidth, std::max(0, screenWidth)),
                       std::min(kChapterBackHitHeight, std::max(0, screenHeight))};
  layout.backVisual = {contentX + 4, headerTop + (layout.headerHeight - 18) / 2, 12, 18};
  layout.header = {contentX, headerTop, contentWidth, layout.headerHeight};
  const int titleX = std::max(contentX + 20, kChapterBackHitWidth + kChapterBackTitleGap);
  layout.headerText = {titleX, headerTop + 6, std::max(0, contentX + contentWidth - titleX),
                       std::max(0, layout.headerHeight - 12)};
  layout.list = {contentX, listTop, contentWidth, listHeight};
  layout.viewport = layout.list;
  layout.firstRow = {layout.list.x, layout.list.y, layout.list.width,
                     std::min(layout.rowHeight, layout.list.height)};
  layout.footer = {0, footerTop, std::max(0, screenWidth),
                   std::max(0, screenHeight - footerTop)};
  return layout;
}

// Standard settings/library-style list row (Fengyan baseline is 40).
inline int listRowHeight(bool touch) { return touch ? 52 : 40; }
inline int listWithSubtitleRowHeight(bool touch) { return touch ? 72 : 60; }

// Chapter TOC / fixed-line menus historically used 30px — too small for fingers.
inline int chapterLineHeight(bool touch) { return touch ? 52 : 30; }
// 5 px top padding + 44 px system header + 16 px list rhythm = 65 px.
// Keep this equal to UITheme/FengyanTheme so drawing and touch hit testing
// share one coordinate system.
inline int chapterListTop(bool touch) { return touch ? 65 : 60; }
inline int chapterTitleY(bool touch) { return touch ? 18 : 15; }

// Touch pagination controls live in a dedicated footer. The page indicator is
// a full-width line above the buttons so values such as 12/30 never get
// truncated by a narrow center slot.
inline int chapterPagerButtonHeight(bool touch) { return touch ? 56 : 0; }
inline int chapterPagerBottomMargin(bool touch) { return touch ? 8 : 0; }
inline int chapterPagerOuterMargin(bool touch) { return touch ? 20 : 0; }
inline int chapterPagerGap(bool touch) { return touch ? 56 : 0; }
inline int chapterPagerLabelHeight(bool touch) { return touch ? 28 : 0; }
inline int chapterPagerLabelGap(bool touch) { return touch ? 4 : 0; }
inline int chapterFooterReserve(bool touch) {
  return touch ? chapterPagerButtonHeight(true) + chapterPagerBottomMargin(true) +
                     chapterPagerLabelGap(true) + chapterPagerLabelHeight(true)
               : 56;  // 40px button-hint bar plus 16px breathing room
}
inline int chapterPagerTop(int screenHeight, bool touch) {
  return screenHeight - chapterPagerBottomMargin(touch) - chapterPagerButtonHeight(touch);
}
inline int chapterPagerButtonWidth(int screenWidth, bool touch) {
  if (!touch) return 0;
  return (screenWidth - 2 * chapterPagerOuterMargin(true) - chapterPagerGap(true)) / 2;
}
inline int chapterPagerLeftX(bool touch) { return touch ? chapterPagerOuterMargin(true) : 0; }
inline int chapterPagerRightX(int screenWidth, bool touch) {
  return touch ? screenWidth - chapterPagerOuterMargin(true) - chapterPagerButtonWidth(screenWidth, true) : 0;
}
inline int chapterPagerLabelTop(int screenHeight, bool touch) {
  if (!touch) return 0;
  return chapterPagerTop(screenHeight, true) - chapterPagerLabelGap(true) - chapterPagerLabelHeight(true);
}

// TXT TOC special chips (向前/向后 100 章)
inline int chapterSpecialTop(bool touch) { return touch ? 48 : 40; }
inline int chapterSpecialHeight(bool touch) { return touch ? 48 : 30; }
// Chapter list begins below the special chip row(s)
inline int chapterBodyTop(bool touch) {
  return chapterSpecialTop(touch) + chapterSpecialHeight(touch) + (touch ? 8 : 10);
}

// Reader menu list start under title/progress
inline int readerMenuListTop(bool touch, int contentY = 0) {
  return (touch ? 80 : 75) + contentY;
}
inline int readerMenuLineHeight(bool touch) { return touch ? 52 : 30; }

// Minimum comfortable tap target (documentation / tests)
constexpr int kMinTouchTargetPx = 48;

inline bool isComfortableTapTarget(int height) { return height >= kMinTouchTargetPx; }

}  // namespace M4TouchListMetrics

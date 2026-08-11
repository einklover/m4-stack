#pragma once

// Touch-friendly list/TOC metrics for M4 (host-testable pure helpers).
// Physical-button builds keep denser rows; touch targets aim ~48–56px.

namespace M4TouchListMetrics {

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
               : 10;
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

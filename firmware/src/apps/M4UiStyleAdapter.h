#pragma once

// Adapter from the firmware's active UITheme to the renderer-independent M4
// style contract. Scenes use this once at entry/rotation and keep the small
// returned value object; they do not query global theme state per row.

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiStyle.h"

namespace M4UiStyleAdapter {

inline M4UiStyle::Theme current(int screenWidth, int screenHeight) {
  const ThemeMetrics& m = UITheme::getInstance().getMetrics();
  M4UiStyle::SystemMetrics sm;
  sm.topPadding = m.topPadding;
  sm.batteryBarHeight = m.batteryBarHeight;
  sm.headerHeight = m.headerHeight;
  sm.verticalSpacing = m.verticalSpacing;
  sm.contentSidePadding = m.contentSidePadding;
  sm.listRowHeight = m.listRowHeight;
  sm.listWithSubtitleRowHeight = m.listWithSubtitleRowHeight;
  sm.buttonHintsHeight = m.buttonHintsHeight;

  M4UiStyle::FontRoles fonts;
  fonts.title = UI_12_FONT_ID;
  fonts.row = UI_12_FONT_ID;
  fonts.subtitle = SMALL_FONT_ID;
  fonts.button = UI_10_FONT_ID;
  fonts.caption = UI_10_FONT_ID;
  return M4UiStyle::makeTheme(screenWidth, screenHeight, sm, fonts);
}

}  // namespace M4UiStyleAdapter

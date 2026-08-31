#pragma once

#include "components/themes/BaseTheme.h"
#include "util/HomeRef.h"

class GfxRenderer;

// 风眼主题指标配置
namespace FengyanMetrics {
constexpr ThemeMetrics values = {
    .batteryWidth = 29,
    .batteryHeight = 13,
    .topPadding = HomeRef::HeaderSafeTop,
    .batteryBarHeight = 46,
    .headerHeight = 46,
    .verticalSpacing = 14,
    .contentSidePadding = 20,
// Murphy M4 is always touch: taller rows for finger targets (see M4TouchListMetrics).
#ifdef CROSSPOINT_MURPHY_M4
    .listRowHeight = 52,
    .listWithSubtitleRowHeight = 72,
#else
    .listRowHeight = 40,
    .listWithSubtitleRowHeight = 60,
#endif
    .menuRowHeight = 72,
    .menuSpacing = 8,
    .tabSpacing = 8,
    .tabBarHeight = 48,
    .scrollBarWidth = 4,
    .scrollBarRightOffset = 5,
    .homeTopPadding = 62,
    .homeCoverHeight = 222,
    .homeCoverTileHeight = 481,
    .homeRecentBooksCount = 4,
    .homeCoverWidth = 171,
    .homeCoverThumbHeight = 254,
    .buttonHintsHeight = 51,
    .sideButtonHintsWidth = 30,
    .versionTextRightX = 20,
    .versionTextY = 55,
    .bookProgressBarHeight = 4
};
}

class FengyanTheme : public BaseTheme {
 public:
  // 组件绘制方法
  void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon,
                const std::function<std::string(int index)>& rowValue) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4, bool force = false) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                           bool force = false) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  // Template Home: draws only dynamic covers/text/progress without static card/divider/cover borders.
  // Used for black-ink overlay compositing: covers are rectangular, template supplies rounded frames.
  void drawRecentBookCoverContent(const GfxRenderer& renderer, Rect rect,
                                  const std::vector<RecentBook>& recentBooks) const;
  void drawRecentBookCoverFocus(const GfxRenderer& renderer, Rect rect,
                                const std::vector<RecentBook>& recentBooks, int selectorIndex) const;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
};

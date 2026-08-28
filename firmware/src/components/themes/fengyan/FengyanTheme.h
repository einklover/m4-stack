#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// 风眼主题指标配置
namespace FengyanMetrics {
constexpr ThemeMetrics values = {
    .batteryWidth = 28,
    .batteryHeight = 18,
    .topPadding = 5,
    .batteryBarHeight = 40,
    .headerHeight = 44,
    .verticalSpacing = 16,
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
    .homeTopPadding = 56,
    .homeCoverHeight = 226,
    .homeCoverTileHeight = 470,  // 封面高度 + 信息框高度 + 间距
    .homeRecentBooksCount = 3,
    .homeCoverWidth = 132,        // availableCoverWidth(140) - 8 = 132
    .homeCoverThumbHeight = 198,  // homeCoverWidth(132) * 3/2 = 198，保扅2:3宣传片比例
    .buttonHintsHeight = 40,
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
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
};

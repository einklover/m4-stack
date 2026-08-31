#include "FengyanTheme.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <HalPowerManager.h>
#include "CrossPointSettings.h"
#include "I18n.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "util/HomeRef.h"
#include "util/M4HomeBookDetailMeta.h"
#include "util/TouchHitGeometry.h"
#include <ctime>
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
// 三套图标风格
#include "components/icons/theme1/folder32.h"
#include "components/icons/theme1/history32.h"
#include "components/icons/theme1/netdisk32.h"
#include "components/icons/theme1/setting32.h"
#include "components/icons/theme1/shuqian32.h"
#include "components/icons/theme1/wifi32.h"
#include "components/icons/theme2/folder32.h"
#include "components/icons/theme2/history32.h"
#include "components/icons/theme2/netdisk32.h"
#include "components/icons/theme2/setting32.h"
#include "components/icons/theme2/shuqian32.h"
#include "components/icons/theme2/wifi32.h"
#include "components/icons/theme3/folder32.h"
#include "components/icons/theme3/history32.h"
#include "components/icons/theme3/netdisk32.h"
#include "components/icons/theme3/setting32.h"
#include "components/icons/theme3/shuqian32.h"
#include "components/icons/theme3/wifi32.h"
#include "components/icons/cog.h"
#include "fontIds.h"
#include "util/M4TouchListMetrics.h"
#include "util/M4TouchNavigation.h"
#include "util/M4UiText.h"
#include "util/StringUtils.h"

// 内部常量
namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 8;  // 风眼主题使用更大的圆角
constexpr int listIconSize = 24;
constexpr int mainMenuIconWidth = 72;   // 主菜单图标宽度
constexpr int mainMenuIconHeight = 72;  // 主菜单图标高度
constexpr int kHomeQuickColumns = 4;
constexpr int kHomeQuickHeaderOffset = 0;

int textTop(const GfxRenderer& renderer, int fontId, int baseline) {
  return baseline - renderer.getLineHeight(fontId);
}

void strokeCircle(const GfxRenderer& renderer, int cx, int cy, int r) {
  if (r < 2) {
    renderer.fillRect(cx, cy, 2, 2, true);
    return;
  }
  renderer.drawRoundedRect(cx - r, cy - r, r * 2 + 1, r * 2 + 1, HomeRef::Stroke, r, true);
}

void drawLineIconFolder(const GfxRenderer& renderer, int x, int y, int s) {
  const int pad = std::max(2, s / 8);
  const int bodyW = s - pad * 2;
  const int tabH = std::max(5, s / 7);
  const int bodyH = s - pad - tabH - 2;
  const int bx = x + pad;
  const int by = y + pad + tabH - 1;
  renderer.drawRect(bx, y + pad, bodyW / 3 + 2, tabH, true);
  renderer.drawRect(bx, by, bodyW, bodyH, true);
}

void drawLineIconWeread(const GfxRenderer& renderer, int x, int y, int s) {
  const int bw = s / 2 + 4;
  const int bh = s / 2 - 2;
  const int r = std::max(5, s / 7);
  renderer.drawRoundedRect(x + 2, y + 2, bw, bh, HomeRef::Stroke, r, true);
  renderer.drawRoundedRect(x + s - bw - 2, y + s - bh - 2, bw, bh, HomeRef::Stroke, r, true);
}

void drawLineIconTomato(const GfxRenderer& renderer, int x, int y, int s) {
  const int cx = x + s / 2;
  const int cy = y + s / 2 + 3;
  const int r = s / 2 - 6;
  strokeCircle(renderer, cx, cy, r);
  renderer.drawLine(cx, y + 2, cx, cy - r, true);
  renderer.drawLine(cx, y + 5, cx - r / 2 - 2, y + 2, true);
  renderer.drawLine(cx, y + 5, cx + r / 2 + 2, y + 2, true);
}

void drawLineIconJinjiang(const GfxRenderer& renderer, int x, int y, int s) {
  auto stemJ = [&](int jx, int top, int bot, int hook) {
    renderer.fillRect(jx, top, 3, 3, true);
    renderer.drawLine(jx, top + 6, jx, bot, true);
    renderer.drawLine(jx + 1, top + 6, jx + 1, bot, true);
    renderer.drawLine(jx, bot, jx - hook, bot, true);
    renderer.drawLine(jx - hook, bot, jx - hook, bot - s / 8, true);
  };
  const int left = x + s / 2 - s / 6;
  const int right = x + s / 2 + s / 6;
  stemJ(left, y + 4, y + s - 6, s / 6);
  stemJ(right, y + 2, y + s - 4, s / 6);
}

void drawWifiGlyph(const GfxRenderer& renderer, int x, int y, int s) {
  const int cx = x + s / 2;
  const int cy = y + s - 4;
  renderer.fillRect(cx, cy, 2, 2, true);
  const int radii[] = {5, 10, 15};
  for (int r : radii) {
    renderer.drawArc(r, cx, cy, -1, -1, HomeRef::Stroke, true);
    renderer.drawArc(r, cx, cy, 1, -1, HomeRef::Stroke, true);
  }
}

std::string firstGlyph(const std::string& s) {
  if (s.empty()) return {};
  const unsigned char c = static_cast<unsigned char>(s[0]);
  size_t n = 1;
  if ((c & 0x80) == 0) n = 1;
  else if ((c & 0xE0) == 0xC0) n = 2;
  else if ((c & 0xF0) == 0xE0) n = 3;
  else if ((c & 0xF8) == 0xF0) n = 4;
  if (n > s.size()) n = s.size();
  return s.substr(0, n);
}

const uint8_t* iconForName(UIIcon icon) {
  // 根据图标风格选择对应的图标
  // iconStyle: 0=风格一, 1=风格二, 2=风格三
  const uint8_t style = SETTINGS.iconStyle;
  
  switch (icon) {
    case UIIcon::Folder:    return Folder24Icon;
    case UIIcon::Book:      return Book24Icon;
    case UIIcon::Text:      return Text24Icon;
    case UIIcon::Image:     return Image24Icon;
    case UIIcon::File:      return File24Icon;
    case UIIcon::Library:   return LibraryIcon;
    case UIIcon::Recent:    return RecentIcon;
    case UIIcon::Settings:  return SettingsIcon;
    case UIIcon::Transfer:  return TransferIcon;
    case UIIcon::Wifi:      return WifiIcon;
    case UIIcon::Hotspot:   return HotspotIcon;
    case UIIcon::Cog:       return CogIcon;
    // 32x32 图标用于 Fengyan 主题菜单（实际尺寸72x72）
    // 根据图标风格选择不同目录的图标
    case UIIcon::Folder32:
      switch (style) {
        case 1: return Folder32Icon_Theme2;
        case 2: return Folder32Icon_Theme3;
        default: return Folder32Icon_Theme1;
      }
    case UIIcon::History32:
      switch (style) {
        case 1: return History32Icon_Theme2;
        case 2: return History32Icon_Theme3;
        default: return History32Icon_Theme1;
      }
    case UIIcon::Netdisk32:
      switch (style) {
        case 1: return Netdisk32Icon_Theme2;
        case 2: return Netdisk32Icon_Theme3;
        default: return Netdisk32Icon_Theme1;
      }
    case UIIcon::Setting32:
      switch (style) {
        case 1: return Setting32Icon_Theme2;
        case 2: return Setting32Icon_Theme3;
        default: return Setting32Icon_Theme1;
      }
    case UIIcon::Wifi32:
      switch (style) {
        case 1: return Wifi32Icon_Theme2;
        case 2: return Wifi32Icon_Theme3;
        default: return Wifi32Icon_Theme1;
      }
    case UIIcon::Shuqian32:
      switch (style) {
        case 1: return Shuqian32Icon_Theme2;
        case 2: return Shuqian32Icon_Theme3;
        default: return Shuqian32Icon_Theme1;
      }
    default:                return nullptr;
  }
}
}  // namespace

void FengyanTheme::drawBattery(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  const uint16_t percentage = powerManager.getBatteryPercentage();
  
  const auto smallFace = M4UiText::resolveSystem(renderer, SMALL_FONT_ID);
  const int fontHeight = renderer.getLineHeight(smallFace.fontId);
  const int batteryYOffset = (fontHeight - rect.height) / 2;
  
  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    M4UiText::drawSystem(renderer, SMALL_FONT_ID,
                          rect.x + batteryPercentSpacing + FengyanMetrics::values.batteryWidth, rect.y,
                          percentageText.c_str());
  }

  const int x = rect.x;
  const int y = rect.y + batteryYOffset;
  const int battWidth = FengyanMetrics::values.batteryWidth - 4;
  const int battHeight = rect.height;

  // 绘制电池外框
  renderer.drawLine(x, y, x + battWidth - 1, y);
  renderer.drawLine(x, y + battHeight - 1, x + battWidth - 1, y + battHeight - 1);
  renderer.drawLine(x, y, x, y + battHeight - 1);
  renderer.drawLine(x + battWidth - 1, y, x + battWidth - 1, y + battHeight - 1);

  // 电池正极凸起
  const int terminalX = x + battWidth;
  const int terminalHeight = battHeight / 2;
  const int terminalY = y + (battHeight - terminalHeight) / 2;
  renderer.fillRect(terminalX, terminalY, 2, terminalHeight);

  // 内边距
  const int innerX = x + 2;
  const int innerY = y + 2;
  const int innerWidth = battWidth - 4;
  const int innerHeight = battHeight - 4;

  // 绘制4段电池电量条
  const int segmentWidth = (innerWidth - 3) / 4;
  const int gap = 1;

  int segmentsToShow = 0;
  if (percentage > 0) segmentsToShow = 1;
  if (percentage > 25) segmentsToShow = 2;
  if (percentage > 50) segmentsToShow = 3;
  if (percentage > 75) segmentsToShow = 4;

  for (int i = 0; i < segmentsToShow; i++) {
    renderer.fillRect(innerX + i * (segmentWidth + gap), innerY, segmentWidth, innerHeight);
  }
}

void FengyanTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool referenceHome = title != nullptr && std::string(title) == "我的mofei";
  if (referenceHome) {
    auto homeTitle = M4UiText::truncatedSystem(renderer, UI_12_FONT_ID, title, 330, EpdFontFamily::BOLD);
    M4UiText::drawSystem(renderer, UI_12_FONT_ID, rect.x + HomeRef::HomeHeaderTitleX,
                         textTop(renderer, UI_12_FONT_ID, rect.y + HomeRef::HomeHeaderTitleBaseline), homeTitle.c_str(), true, EpdFontFamily::BOLD);
    drawWifiGlyph(renderer, rect.x + HomeRef::HomeHeaderWifiX, rect.y + (HomeRef::HomeHeaderH - HomeRef::HomeHeaderIcon) / 2, HomeRef::HomeHeaderIcon);
    renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, 1, true);
    return;
  }
  const int dy = rect.y - HomeRef::HeaderY;
  const int battY = rect.y + (HomeRef::HeaderH - HomeRef::HeaderBatteryH) / 2;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  if (showBatteryPercentage) {
    const uint16_t percentage = powerManager.getBatteryPercentage();
    const auto percentageText = std::to_string(percentage) + "%";
    M4UiText::drawSystem(renderer, SMALL_FONT_ID, rect.x + HomeRef::HeaderBatteryTextX,
                         textTop(renderer, SMALL_FONT_ID, HomeRef::HeaderTitleBaseline + dy),
                         percentageText.c_str());
  }
  drawBattery(renderer,
              Rect{rect.x + HomeRef::HeaderBatteryX, battY, HomeRef::HeaderBatteryW, HomeRef::HeaderBatteryH},
              false);

  drawWifiGlyph(renderer, rect.x + HomeRef::HeaderWifiX, rect.y + (HomeRef::HeaderH - HomeRef::HeaderIcon) / 2,
                HomeRef::HeaderIcon);

  time_t now = time(nullptr);
  struct tm local {};
  if (now > 0 && localtime_r(&now, &local) != nullptr) {
    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
    M4UiText::drawSystem(renderer, SMALL_FONT_ID, rect.x + HomeRef::HeaderTimeX,
                         textTop(renderer, SMALL_FONT_ID, HomeRef::HeaderTitleBaseline + dy), clock);
  }

  renderer.drawLine(rect.x + HomeRef::HeaderDividerX, rect.y + 12,
                    rect.x + HomeRef::HeaderDividerX, rect.y + rect.height - 12, 1, true);

  if (title) {
    const int titleX = rect.x + HomeRef::HeaderTitleX;
    const int titleMaxWidth = HomeRef::HeaderTimeX - HomeRef::HeaderTitleX - 8;
    auto truncatedTitle = M4UiText::truncatedSystem(renderer, UI_12_FONT_ID, title,
                                                     std::max(1, titleMaxWidth), EpdFontFamily::BOLD);
    M4UiText::drawSystem(renderer, UI_12_FONT_ID, titleX,
                         textTop(renderer, UI_12_FONT_ID, HomeRef::HeaderTitleBaseline + dy),
                         truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
  }
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, 1, true);
}

void FengyanTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                              bool selected) const {
  int currentX = rect.x + FengyanMetrics::values.contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  for (const auto& tab : tabs) {
    const int textWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * hPaddingInSelection, rect.height - 4,
                                 cornerRadius, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * hPaddingInSelection, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    M4UiText::draw(renderer, UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label,
                   !(tab.selected && selected), EpdFontFamily::REGULAR);

    currentX += textWidth + FengyanMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width, rect.y + rect.height - 1, true);
}

void FengyanTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle,
                            const std::function<std::string(int index)>& rowSubtitle,
                            const std::function<UIIcon(int index)>& rowIcon,
                            const std::function<std::string(int index)>& rowValue) const {
  const bool hasSubtitle = rowSubtitle != nullptr;
  int rowHeight = M4UiText::listRowHeight(
      renderer, UI_10_FONT_ID,
      hasSubtitle ? FengyanMetrics::values.listWithSubtitleRowHeight : FengyanMetrics::values.listRowHeight,
      hasSubtitle);
  // Avoid divide-by-zero when the rect is shorter than one row (tight landscape gutters).
  int pageItems = std::max(1, rowHeight > 0 ? rect.height / rowHeight : 1);

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - FengyanMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - FengyanMetrics::values.scrollBarWidth, scrollBarY, FengyanMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  int contentWidth =
      rect.width -
      (totalPages > 1 ? (FengyanMetrics::values.scrollBarWidth + FengyanMetrics::values.scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(FengyanMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - FengyanMetrics::values.contentSidePadding * 2, rowHeight, cornerRadius,
                             Color::LightGray);
  }

  int textX = rect.x + FengyanMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - FengyanMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  if (rowIcon != nullptr) {
    textX += listIconSize + hPaddingInSelection;
    textWidth -= listIconSize + hPaddingInSelection;
  }

  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  // Align to the title/subtitle block, not merely to the touch row. This
  // keeps title-only and two-line rows on the same visual baseline.
  const int subtitleTop = M4UiText::listSubtitleTop(renderer, UI_10_FONT_ID);
  const int iconYOffset = M4UiText::listIconTop(renderer, UI_10_FONT_ID, rowHeight,
                                                hasSubtitle, listIconSize, 4, subtitleTop);
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, rect.x + FengyanMetrics::values.contentSidePadding + hPaddingInSelection,
                          itemY + iconYOffset, listIconSize, listIconSize);
      }
    }

    int rowTextWidth = textWidth - (rowValue != nullptr ? 100 : 0);
    auto itemName = rowTitle(i);
    auto item = M4UiText::truncated(renderer, UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    M4UiText::draw(renderer, UI_10_FONT_ID, textX, itemY + 4, item.c_str(), true);

    if (hasSubtitle) {
      std::string subtitleText = rowSubtitle(i);
      // Subtitle chrome: still compact; use UI face path so CJK is covered.
      auto subtitle = M4UiText::truncated(renderer, UI_10_FONT_ID, subtitleText.c_str(), rowTextWidth);
      M4UiText::draw(renderer, UI_10_FONT_ID, textX, itemY + subtitleTop, subtitle.c_str(), true);
    }

    if (rowValue != nullptr) {
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        // 关键修复：限制右侧值的最大宽度为240像素，超出则截断
        constexpr int maxValueWidth = 240;
        auto truncatedValue = M4UiText::truncated(renderer, UI_10_FONT_ID, valueText.c_str(), maxValueWidth);
        const auto valueTextWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, truncatedValue.c_str());

        if (i == selectedIndex) {
          // 选中时显示黑色填充背景
          renderer.fillRoundedRect(
              contentWidth - FengyanMetrics::values.contentSidePadding - hPaddingInSelection * 2 - valueTextWidth, itemY,
              valueTextWidth + hPaddingInSelection * 2, rowHeight, cornerRadius, Color::Black);
        }

        // 根据是否选中决定文字颜色
        const bool isWhiteText = (i == selectedIndex);
        // 关键修复：">"符号未选中时使用深灰色
        const bool isGrayArrow = (truncatedValue == ">" && !isWhiteText);
        
        if (isGrayArrow) {
          M4UiText::draw(renderer, UI_10_FONT_ID,
                         contentWidth - FengyanMetrics::values.contentSidePadding - hPaddingInSelection - valueTextWidth,
                         itemY + 4, truncatedValue.c_str(), false);
        } else {
          M4UiText::draw(renderer, UI_10_FONT_ID,
                         contentWidth - FengyanMetrics::values.contentSidePadding - hPaddingInSelection - valueTextWidth,
                         itemY + 4, truncatedValue.c_str(), !isWhiteText);
        }
      }
    }
  }
}

void FengyanTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4, bool force) const {
  if (!force && !SETTINGS.buttonHintsEnabled) return;

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const bool homeFooter = force && btn1 && btn2 && btn3 && btn4 && btn4[0] == 0 &&
                          std::string(btn1) == "历史" && std::string(btn2) == "应用" &&
                          std::string(btn3) == "设置";

  const int pageHeight = renderer.getScreenHeight();
  if (homeFooter) {
    const int top = HomeRef::BottomY;
    renderer.fillRect(0, top, HomeRef::ScreenW, HomeRef::BottomH, false);
    renderer.drawLine(0, top, HomeRef::ScreenW - 1, top, 1, true);
    renderer.drawLine(HomeRef::BottomSplit1, top, HomeRef::BottomSplit1, pageHeight - 1, 1, true);
    renderer.drawLine(HomeRef::BottomSplit2, top, HomeRef::BottomSplit2, pageHeight - 1, 1, true);
    const char* homeLabels[] = {btn1, btn2, btn3};
    const int xs[] = {0, HomeRef::BottomSplit1, HomeRef::BottomSplit2};
    const int ws[] = {HomeRef::BottomSplit1, HomeRef::BottomSplit2 - HomeRef::BottomSplit1,
                      HomeRef::ScreenW - HomeRef::BottomSplit2};
    for (int i = 0; i < 3; ++i) {
      const int textWidth = M4UiText::systemTextWidth(renderer, SMALL_FONT_ID, homeLabels[i]);
      const int textX = xs[i] + (ws[i] - textWidth) / 2;
      const int textY = textTop(renderer, SMALL_FONT_ID, HomeRef::BottomBaseline);
      M4UiText::drawSystem(renderer, SMALL_FONT_ID, textX, textY, homeLabels[i]);
    }
    renderer.setOrientation(orig_orientation);
    return;
  }

  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = FengyanMetrics::values.buttonHintsHeight;
  constexpr int buttonY = FengyanMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  constexpr int buttonPositions[] = {38, 154, 268, 384};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      const int textWidth = M4UiText::systemTextWidth(renderer, SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      const int textY = pageHeight - buttonY + textYOffset;
      M4UiText::drawSystem(renderer, SMALL_FONT_ID, textX, textY, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void FengyanTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                                       bool force) const {
  if (!force && !SETTINGS.buttonHintsEnabled) return;

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = FengyanMetrics::values.sideButtonHintsWidth;
  constexpr int buttonHeight = 80;
  constexpr int buttonMargin = 4;
  constexpr int x3ButtonY = 155;

  if (topBtn != nullptr && topBtn[0] != '\0') {
    const int leftX = buttonMargin;
    renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
    const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    const int textX = leftX + (buttonWidth - textHeight) / 2;
    const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
    renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
  }

  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    const int rightX = screenWidth - buttonMargin - buttonWidth;
    renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
    const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    const int textX = rightX + (buttonWidth - textHeight) / 2;
    const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
    renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
  }
}

// Reference-style Home: one prominent recent book, followed by three compact recent books.
void FengyanTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  const int bookCount =
      std::min(static_cast<int>(recentBooks.size()), FengyanMetrics::values.homeRecentBooksCount);
  const auto layout = TouchHitGeometry::makeFengyanRecentLayout(
      TouchHitGeometry::Rect{rect.x, rect.y, rect.width, rect.height}, bookCount,
      FengyanMetrics::values.contentSidePadding);
  const int dy = rect.y - HomeRef::Recent.y;

  auto drawCard = [&]() {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    renderer.drawRoundedRect(layout.panel.x, layout.panel.y, layout.panel.width, layout.panel.height,
                             HomeRef::Stroke, HomeRef::CardRadius, true);
  };

  auto drawCover = [&](int bookIndex, const TouchHitGeometry::Rect& dst, const char* title) {
    bool drawn = false;
    if (bookIndex >= 0 && bookIndex < bookCount && !recentBooks[bookIndex].coverBmpPath.empty()) {
      const std::string thumbPath =
          UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath,
                                     FengyanMetrics::values.homeCoverWidth,
                                     FengyanMetrics::values.homeCoverThumbHeight);
      FsFile file;
      if (SdMan.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, dst.x, dst.y, dst.width, dst.height);
          drawn = true;
        }
        file.close();
      }
    }
    renderer.drawRoundedRect(dst.x, dst.y, dst.width, dst.height, HomeRef::Stroke, HomeRef::CoverRadius, true);
    if (!drawn) {
      renderer.fillRect(dst.x + 1, dst.y + 1, dst.width - 2, dst.height - 2, false);
      const std::string glyph = firstGlyph(title ? title : "");
      if (!glyph.empty()) {
        const int tw = M4UiText::textWidth(renderer, UI_12_FONT_ID, glyph.c_str(), EpdFontFamily::BOLD);
        M4UiText::draw(renderer, UI_12_FONT_ID, dst.x + (dst.width - tw) / 2,
                       dst.y + (dst.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2, glyph.c_str(), true,
                       EpdFontFamily::BOLD);
      } else {
        renderer.drawRect(dst.x + dst.width / 2 - 8, dst.y + dst.height / 2 - 12, 16, 24, true);
      }
    }
  };

  if (recentBooks.empty()) {
    if (!coverRendered) {
      drawCard();
      M4UiText::draw(renderer, UI_12_FONT_ID, rect.x + HomeRef::RecentTitleX,
                     textTop(renderer, UI_12_FONT_ID, HomeRef::RecentTitleBaseline + dy), "最近阅读", true,
                     EpdFontFamily::BOLD);
      const char* countText = "0/0";
      const int countWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, countText);
      M4UiText::draw(renderer, UI_10_FONT_ID, rect.x + HomeRef::RecentCountRight - countWidth,
                     textTop(renderer, UI_10_FONT_ID, HomeRef::RecentCountBaseline + dy), countText, true);
      const char* emptyMsg = L(Str::kNoReadingHistory);
      const int textW = M4UiText::textWidth(renderer, UI_10_FONT_ID, emptyMsg);
      const int textH = renderer.getLineHeight(UI_10_FONT_ID);
      M4UiText::draw(renderer, UI_10_FONT_ID, layout.panel.x + (layout.panel.width - textW) / 2,
                     layout.panel.y + (layout.panel.height - textH) / 2, emptyMsg, true);
      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }
    return;
  }
  if (!layout.valid()) return;

  if (!coverRendered) {
    drawCard();
    M4UiText::draw(renderer, UI_12_FONT_ID, rect.x + HomeRef::RecentTitleX,
                   textTop(renderer, UI_12_FONT_ID, HomeRef::RecentTitleBaseline + dy), "最近阅读", true,
                   EpdFontFamily::BOLD);
    const std::string countText = std::to_string(bookCount) + "/" + std::to_string(recentBooks.size());
    const int countWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, countText.c_str());
    M4UiText::draw(renderer, UI_10_FONT_ID, rect.x + HomeRef::RecentCountRight - countWidth,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::RecentCountBaseline + dy), countText.c_str(), true);

    const auto& heroBook = recentBooks[0];
    drawCover(0, layout.heroCover, heroBook.title.c_str());
    const auto heroMeta = M4HomeBookDetailMeta::presentCached(
        heroBook.path, heroBook.title, heroBook.author, heroBook.progress,
        {L(Str::kValNone), L(Str::kBookSourceLocal), L(Str::kBookSourceUnknown)});

    const int infoX = rect.x + HomeRef::HeroTextX;
    const int infoW = HomeRef::HeroTextRight - HomeRef::HeroTextX;
    auto heroTitle = M4UiText::truncated(renderer, UI_12_FONT_ID, heroMeta.title.c_str(),
                                         infoW, EpdFontFamily::BOLD);
    M4UiText::draw(renderer, UI_12_FONT_ID, infoX,
                   textTop(renderer, UI_12_FONT_ID, HomeRef::HeroTitleBaseline + dy), heroTitle.c_str(), true,
                   EpdFontFamily::BOLD);

    auto heroAuthor = M4UiText::truncated(renderer, UI_10_FONT_ID, heroMeta.author.c_str(), infoW);
    M4UiText::draw(renderer, UI_10_FONT_ID, infoX,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::HeroAuthorBaseline + dy), heroAuthor.c_str(), true);

    const std::string sourceText = std::string("来源：") + heroMeta.source;
    auto heroSource = M4UiText::truncated(renderer, UI_10_FONT_ID, sourceText.c_str(), infoW);
    M4UiText::draw(renderer, UI_10_FONT_ID, infoX,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::HeroSourceBaseline + dy), heroSource.c_str(), true);

    const int progress = std::max(0, std::min(100, heroBook.progress));
    const std::string progressText = std::to_string(progress) + "%";
    M4UiText::draw(renderer, UI_10_FONT_ID, infoX,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::HeroProgressBaseline + dy), progressText.c_str(),
                   true);
    const int barY = layout.progress.y;
    const int barH = layout.progress.height;
    renderer.fillRect(layout.progress.x, barY, layout.progress.width, barH, false);
    renderer.drawRoundedRect(layout.progress.x, barY, layout.progress.width, barH, HomeRef::Stroke, barH / 2, true);
    if (progress > 0 && layout.progress.width > 4 && barH > 4) {
      const int innerW = layout.progress.width - 4;
      const int fillW = std::max(barH - 4, (innerW * progress) / 100);
      renderer.fillRoundedRect(layout.progress.x + 2, barY + 2, std::min(innerW, fillW), barH - 4, (barH - 4) / 2, Color::Black);
    }

    renderer.drawLine(rect.x + HomeRef::HeroDividerX1, layout.dividerY, rect.x + HomeRef::HeroDividerX2,
                      layout.dividerY, 1, true);

    const int miniCenters[3] = {HomeRef::MiniTitleCenter1, HomeRef::MiniTitleCenter2, HomeRef::MiniTitleCenter3};
    for (int i = 1; i < bookCount; ++i) {
      const int miniIndex = i - 1;
      drawCover(i, layout.miniCover[miniIndex], recentBooks[i].title.c_str());
      auto title = M4UiText::truncated(renderer, UI_10_FONT_ID, recentBooks[i].title.c_str(),
                                       layout.miniCover[miniIndex].width);
      const int titleW = M4UiText::textWidth(renderer, UI_10_FONT_ID, title.c_str());
      M4UiText::draw(renderer, UI_10_FONT_ID, rect.x + miniCenters[miniIndex] - titleW / 2,
                     textTop(renderer, UI_10_FONT_ID, HomeRef::MiniTitleBaseline + dy), title.c_str(), true);
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = true;
  }

  if (selectorIndex >= 0 && selectorIndex < bookCount) {
    const auto selected =
        selectorIndex == 0 ? layout.heroCover : layout.miniCover[std::max(0, selectorIndex - 1)];
    const int inset = HomeRef::FocusInset + 1;
    renderer.drawRoundedRect(selected.x + inset, selected.y + inset, selected.width - inset * 2,
                             selected.height - inset * 2, HomeRef::Stroke, HomeRef::CoverRadius, true);
  }
}

void FengyanTheme::drawRecentBookCoverContent(const GfxRenderer& renderer, Rect rect,
                                              const std::vector<RecentBook>& recentBooks) const {
  const int bookCount =
      std::min(static_cast<int>(recentBooks.size()), FengyanMetrics::values.homeRecentBooksCount);
  const auto layout = TouchHitGeometry::makeFengyanRecentLayout(
      TouchHitGeometry::Rect{rect.x, rect.y, rect.width, rect.height}, bookCount,
      FengyanMetrics::values.contentSidePadding);
  const int dy = rect.y - HomeRef::Recent.y;

  auto drawCoverRectangular = [&](int bookIndex, const TouchHitGeometry::Rect& dst, const char* title) {
    bool drawn = false;
    if (bookIndex >= 0 && bookIndex < bookCount && !recentBooks[bookIndex].coverBmpPath.empty()) {
      const std::string thumbPath =
          UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath,
                                     FengyanMetrics::values.homeCoverWidth,
                                     FengyanMetrics::values.homeCoverThumbHeight);
      FsFile file;
      if (SdMan.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, dst.x, dst.y, dst.width, dst.height);
          drawn = true;
        }
        file.close();
      }
    }
    // Template supplies rounded frame ink last — draw only rectangular content, no border.
    if (!drawn) {
      renderer.fillRect(dst.x, dst.y, dst.width, dst.height, false);
      // Keep fallback glyph centered, no border.
      const std::string glyph = firstGlyph(title ? title : "");
      if (!glyph.empty()) {
        const int tw = M4UiText::textWidth(renderer, UI_12_FONT_ID, glyph.c_str(), EpdFontFamily::BOLD);
        M4UiText::draw(renderer, UI_12_FONT_ID, dst.x + (dst.width - tw) / 2,
                       dst.y + (dst.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2, glyph.c_str(), true,
                       EpdFontFamily::BOLD);
      }
    }
  };

  if (recentBooks.empty()) {
    // Template owns card chrome and "最近阅读" label — draw only dynamic count + empty message.
    const char* countText = "0 本";
    const int countWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, countText);
    M4UiText::draw(renderer, UI_10_FONT_ID, rect.x + HomeRef::RecentCountRight - countWidth,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::RecentCountBaseline + dy), countText, true);
    const char* emptyMsg = L(Str::kNoReadingHistory);
    const int textW = M4UiText::textWidth(renderer, UI_10_FONT_ID, emptyMsg);
    const int textH = renderer.getLineHeight(UI_10_FONT_ID);
    M4UiText::draw(renderer, UI_10_FONT_ID, layout.panel.x + (layout.panel.width - textW) / 2,
                   layout.panel.y + (layout.panel.height - textH) / 2, emptyMsg, true);
    return;
  }
  if (!layout.valid()) return;

  // Dynamic recent-count (template has no count text)
  {
    const std::string countText = std::to_string(recentBooks.size()) + " 本";
    const int countWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, countText.c_str());
    M4UiText::draw(renderer, UI_10_FONT_ID, rect.x + HomeRef::RecentCountRight - countWidth,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::RecentCountBaseline + dy), countText.c_str(), true);
  }

  const auto& heroBook = recentBooks[0];
  drawCoverRectangular(0, layout.heroCover, heroBook.title.c_str());

  const auto heroMeta = M4HomeBookDetailMeta::presentCached(
      heroBook.path, heroBook.title, heroBook.author, heroBook.progress,
      {L(Str::kValNone), L(Str::kBookSourceLocal), L(Str::kBookSourceUnknown)});

  const int infoX = rect.x + HomeRef::HeroTextX;
  const int infoW = HomeRef::HeroTextRight - HomeRef::HeroTextX;
  auto heroTitle = M4UiText::truncated(renderer, UI_12_FONT_ID, heroMeta.title.c_str(),
                                       infoW, EpdFontFamily::BOLD);
  M4UiText::draw(renderer, UI_12_FONT_ID, infoX,
                 textTop(renderer, UI_12_FONT_ID, HomeRef::HeroTitleBaseline + dy), heroTitle.c_str(), true,
                 EpdFontFamily::BOLD);

  auto heroAuthor = M4UiText::truncated(renderer, UI_10_FONT_ID, heroMeta.author.c_str(), infoW);
  M4UiText::draw(renderer, UI_10_FONT_ID, infoX,
                 textTop(renderer, UI_10_FONT_ID, HomeRef::HeroAuthorBaseline + dy), heroAuthor.c_str(), true);

  const std::string sourceText = std::string("来源：") + heroMeta.source;
  auto heroSource = M4UiText::truncated(renderer, UI_10_FONT_ID, sourceText.c_str(), infoW);
  M4UiText::draw(renderer, UI_10_FONT_ID, infoX,
                 textTop(renderer, UI_10_FONT_ID, HomeRef::HeroSourceBaseline + dy), heroSource.c_str(), true);

  const int progress = std::max(0, std::min(100, heroBook.progress));
  const std::string progressText = std::to_string(progress) + "%";
  M4UiText::draw(renderer, UI_10_FONT_ID, infoX,
                 textTop(renderer, UI_10_FONT_ID, HomeRef::HeroProgressBaseline + dy), progressText.c_str(),
                 true);
  // Template has no progress outline — draw dynamic bar.
  const int barY = layout.progress.y;
  const int barH = layout.progress.height;
  renderer.fillRect(layout.progress.x, barY, layout.progress.width, barH, false);
  renderer.drawRoundedRect(layout.progress.x, barY, layout.progress.width, barH, HomeRef::Stroke, barH / 2, true);
  if (progress > 0 && layout.progress.width > 4 && barH > 4) {
    const int innerW = layout.progress.width - 4;
    const int fillW = std::max(barH - 4, (innerW * progress) / 100);
    renderer.fillRoundedRect(layout.progress.x + 2, barY + 2, std::min(innerW, fillW), barH - 4, (barH - 4) / 2, Color::Black);
  }

  const int miniCenters[3] = {HomeRef::MiniTitleCenter1, HomeRef::MiniTitleCenter2, HomeRef::MiniTitleCenter3};
  for (int i = 1; i < bookCount; ++i) {
    const int miniIndex = i - 1;
    drawCoverRectangular(i, layout.miniCover[miniIndex], recentBooks[i].title.c_str());
    auto title = M4UiText::truncated(renderer, UI_10_FONT_ID, recentBooks[i].title.c_str(),
                                     layout.miniCover[miniIndex].width);
    const int titleW = M4UiText::textWidth(renderer, UI_10_FONT_ID, title.c_str());
    M4UiText::draw(renderer, UI_10_FONT_ID, rect.x + miniCenters[miniIndex] - titleW / 2,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::MiniTitleBaseline + dy), title.c_str(), true);
    // Template owns no mini author — skip to avoid stale text; dynamic title is enough.
  }
}

void FengyanTheme::drawRecentBookCoverFocus(const GfxRenderer& renderer, Rect rect,
                                            const std::vector<RecentBook>& recentBooks, int selectorIndex) const {
  const int bookCount =
      std::min(static_cast<int>(recentBooks.size()), FengyanMetrics::values.homeRecentBooksCount);
  const auto layout = TouchHitGeometry::makeFengyanRecentLayout(
      TouchHitGeometry::Rect{rect.x, rect.y, rect.width, rect.height}, bookCount,
      FengyanMetrics::values.contentSidePadding);
  if (!layout.valid()) return;
  if (selectorIndex < 0 || selectorIndex >= bookCount) return;
  const auto selected =
      selectorIndex == 0 ? layout.heroCover : layout.miniCover[std::max(0, selectorIndex - 1)];
  const int inset = HomeRef::FocusInset + 1;
  renderer.drawRoundedRect(selected.x + inset, selected.y + inset, selected.width - inset * 2,
                           selected.height - inset * 2, HomeRef::Stroke, HomeRef::CoverRadius, true);
}


// 风眼主题首页快捷操作：四列网格，标题与触摸布局共用偏移
void FengyanTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const int dy = rect.y - HomeRef::Quick.y;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawRoundedRect(rect.x + HomeRef::Quick.x, rect.y, HomeRef::Quick.w, HomeRef::Quick.h, HomeRef::Stroke,
                           HomeRef::CardRadius, true);

  const auto layout = TouchHitGeometry::makeFengyanMenuLayout(
      TouchHitGeometry::Rect{rect.x, rect.y, rect.width, rect.height}, buttonCount,
      FengyanMetrics::values.contentSidePadding, kHomeQuickHeaderOffset, kHomeQuickColumns);
  if (!layout.valid()) return;

  const int iconSize = HomeRef::QuickIconSize;
  const int iconY = HomeRef::QuickIconY + dy;
  for (int i = 0; i < layout.buttonCount; ++i) {
    const auto tr = layout.tileRect(i);
    Rect tileRect = Rect{tr.x, tr.y, tr.width, tr.height};
    const bool selected = selectedIndex == i;

    renderer.drawRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, HomeRef::Stroke,
                             HomeRef::TileRadius, true);
    if (selected) {
      const int inset = HomeRef::FocusInset + 1;
      renderer.drawRoundedRect(tileRect.x + inset, tileRect.y + inset, tileRect.width - inset * 2,
                               tileRect.height - inset * 2, HomeRef::Stroke, HomeRef::TileRadius, true);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, label);
    const int iconX = tileRect.x + (tileRect.width - iconSize) / 2;
    if (i == 0) drawLineIconFolder(renderer, iconX, iconY, iconSize);
    else if (i == 1) drawLineIconWeread(renderer, iconX, iconY, iconSize);
    else if (i == 2) drawLineIconTomato(renderer, iconX, iconY, iconSize);
    else if (i == 3) drawLineIconJinjiang(renderer, iconX, iconY, iconSize);
    else if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      if (icon == UIIcon::Apps32) {
        const int cell = 4;
        const int gap = 3;
        const int grid = cell * 3 + gap * 2;
        const int gx = iconX + (iconSize - grid) / 2;
        const int gy = iconY + (iconSize - grid) / 2;
        for (int row = 0; row < 3; ++row) {
          for (int col = 0; col < 3; ++col) {
            renderer.fillRect(gx + col * (cell + gap), gy + row * (cell + gap), cell, cell, true);
          }
        }
      }
    }
    (void)rowIcon;

    const int textX = tileRect.x + (tileRect.width - textWidth) / 2;
    M4UiText::draw(renderer, UI_10_FONT_ID, textX,
                   textTop(renderer, UI_10_FONT_ID, HomeRef::QuickLabelBaseline + dy), label, true);
  }
}

Rect FengyanTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int margin = 15;
  constexpr int y = 60;
  const int textWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, message, EpdFontFamily::REGULAR);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + margin * 2;
  const int h = textHeight + margin * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, false);
  renderer.drawRect(x, y, w, h, true);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + margin - 2;
  M4UiText::draw(renderer, UI_12_FONT_ID, textX, textY, message, true, EpdFontFamily::REGULAR);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

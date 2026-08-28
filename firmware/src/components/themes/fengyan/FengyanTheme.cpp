#include "FengyanTheme.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <HalPowerManager.h>
#include "CrossPointSettings.h"
#include "I18n.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "util/M4HomeBookDetailMeta.h"
#include "util/TouchHitGeometry.h"
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
constexpr int kHomeQuickHeaderOffset = 44;

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
  // 风眼主题：顶部状态栏简洁设计
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  int batteryX = rect.x + rect.width - FengyanMetrics::values.contentSidePadding - FengyanMetrics::values.batteryWidth;
  if (showBatteryPercentage) {
    const uint16_t percentage = powerManager.getBatteryPercentage();
    const auto percentageText = std::to_string(percentage) + "%";
    batteryX -= M4UiText::systemTextWidth(renderer, SMALL_FONT_ID, percentageText.c_str());
  }
  drawBattery(renderer,
              Rect{batteryX, rect.y + 6, FengyanMetrics::values.batteryWidth, FengyanMetrics::values.batteryHeight},
              showBatteryPercentage);

  if (title) {
    const int titleX = M4TouchNavigation::enabled()
                           ? std::max(rect.x + FengyanMetrics::values.contentSidePadding,
                                      M4TouchNavigation::kHeaderHitWidth +
                                          M4TouchListMetrics::kChapterBackTitleGap)
                           : rect.x + FengyanMetrics::values.contentSidePadding;
    const int titleMaxWidth = batteryX - titleX - FengyanMetrics::values.contentSidePadding;
    auto truncatedTitle = M4UiText::truncatedSystem(renderer, UI_12_FONT_ID, title,
                                                     std::max(1, titleMaxWidth), EpdFontFamily::BOLD);
    M4UiText::drawSystem(renderer, UI_12_FONT_ID, titleX, rect.y + 6, truncatedTitle.c_str(), true,
                         EpdFontFamily::BOLD);
    // 底部细线分隔
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width, rect.y + rect.height - 3, 1, true);
  }
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

  const int pageHeight = renderer.getScreenHeight();
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
      M4UiText::drawSystem(renderer, SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
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

// Reference-style Home: one prominent recent book, followed by two compact recent books.
void FengyanTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  const int bookCount =
      std::min(static_cast<int>(recentBooks.size()), FengyanMetrics::values.homeRecentBooksCount);
  const auto layout = TouchHitGeometry::makeFengyanRecentLayout(
      TouchHitGeometry::Rect{rect.x, rect.y, rect.width, rect.height}, bookCount,
      FengyanMetrics::values.contentSidePadding);
  if (!layout.valid()) return;

  auto drawCover = [&](int bookIndex, const TouchHitGeometry::Rect& dst) {
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
    if (!drawn) {
      renderer.fillRect(dst.x, dst.y, dst.width, dst.height, false);
      renderer.drawRect(dst.x, dst.y, dst.width, dst.height, true);
      const int iconSize = std::min(32, std::min(dst.width - 8, dst.height - 8));
      if (iconSize > 0) {
        renderer.drawIcon(CoverIcon, dst.x + (dst.width - iconSize) / 2,
                          dst.y + (dst.height - iconSize) / 2, iconSize, iconSize);
      }
    } else {
      renderer.drawRect(dst.x, dst.y, dst.width, dst.height, true);
    }
  };

  if (!coverRendered) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    renderer.drawRect(layout.panel.x, layout.panel.y, layout.panel.width, layout.panel.height, true);

    M4UiText::draw(renderer, UI_12_FONT_ID, layout.panel.x + 18, layout.panel.y + 14,
                   "最近阅读", true, EpdFontFamily::BOLD);
    const std::string countText = std::to_string(bookCount) + "/" + std::to_string(recentBooks.size());
    const int countWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, countText.c_str());
    M4UiText::draw(renderer, UI_10_FONT_ID, layout.panel.x + layout.panel.width - countWidth - 18,
                   layout.panel.y + 16, countText.c_str(), true);

    const auto& heroBook = recentBooks[0];
    drawCover(0, layout.heroCover);
    const auto heroMeta = M4HomeBookDetailMeta::presentCached(
        heroBook.path, heroBook.title, heroBook.author, heroBook.progress,
        {L(Str::kValNone), L(Str::kBookSourceLocal), L(Str::kBookSourceUnknown)});

    const int infoX = layout.heroInfo.x;
    const int infoW = layout.heroInfo.width;
    auto heroTitle = M4UiText::truncated(renderer, UI_12_FONT_ID, heroMeta.title.c_str(),
                                         infoW, EpdFontFamily::BOLD);
    M4UiText::draw(renderer, UI_12_FONT_ID, infoX, layout.heroInfo.y, heroTitle.c_str(),
                   true, EpdFontFamily::BOLD);

    auto heroAuthor = M4UiText::truncated(renderer, UI_10_FONT_ID, heroMeta.author.c_str(), infoW);
    M4UiText::draw(renderer, UI_10_FONT_ID, infoX, layout.heroInfo.y + 44, heroAuthor.c_str(), true);

    const std::string sourceText = std::string("来源：") + heroMeta.source;
    auto heroSource = M4UiText::truncated(renderer, UI_10_FONT_ID, sourceText.c_str(), infoW);
    M4UiText::draw(renderer, UI_10_FONT_ID, infoX, layout.heroInfo.y + 78, heroSource.c_str(), true);

    const int progress = std::max(0, std::min(100, heroBook.progress));
    const std::string progressText = std::to_string(progress) + "%";
    M4UiText::draw(renderer, UI_10_FONT_ID, infoX, layout.progress.y - 24, progressText.c_str(), true);
    renderer.fillRect(layout.progress.x, layout.progress.y, layout.progress.width, layout.progress.height, false);
    renderer.drawRect(layout.progress.x, layout.progress.y, layout.progress.width, layout.progress.height, true);
    if (progress > 0 && layout.progress.width > 4) {
      const int fillW = ((layout.progress.width - 4) * progress) / 100;
      renderer.fillRect(layout.progress.x + 2, layout.progress.y + 2, fillW,
                        std::max(1, layout.progress.height - 4), true);
    }

    renderer.drawLine(layout.panel.x + 16, layout.dividerY,
                      layout.panel.x + layout.panel.width - 17, layout.dividerY, 1, true);

    for (int i = 1; i < bookCount; ++i) {
      const int miniIndex = i - 1;
      drawCover(i, layout.miniCover[miniIndex]);
      auto title = M4UiText::truncated(renderer, UI_10_FONT_ID, recentBooks[i].title.c_str(),
                                       layout.mini[miniIndex].width);
      const int titleW = M4UiText::textWidth(renderer, UI_10_FONT_ID, title.c_str());
      M4UiText::draw(renderer, UI_10_FONT_ID,
                     layout.mini[miniIndex].x + (layout.mini[miniIndex].width - titleW) / 2,
                     layout.miniCover[miniIndex].y + layout.miniCover[miniIndex].height + 8,
                     title.c_str(), true);
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = true;
  }

  if (selectorIndex >= 0 && selectorIndex < bookCount) {
    const auto selected = layout.bookRect(selectorIndex);
    renderer.drawRect(selected.x - 2, selected.y - 2, selected.width + 4, selected.height + 4, true);
  }
}


// 风眼主题首页快捷操作：四列网格，标题与触摸布局共用偏移
void FengyanTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const int panelInset = std::max(12, FengyanMetrics::values.contentSidePadding - 6);
  renderer.drawRect(rect.x + panelInset, rect.y, rect.width - panelInset * 2, rect.height, true);
  M4UiText::draw(renderer, UI_12_FONT_ID, rect.x + panelInset + 18, rect.y + 14,
                 "快捷操作", true, EpdFontFamily::BOLD);

  // Reference quick actions: four columns below a compact section header.
  const auto layout = TouchHitGeometry::makeFengyanMenuLayout(
      TouchHitGeometry::Rect{rect.x, rect.y, rect.width, rect.height}, buttonCount,
      FengyanMetrics::values.contentSidePadding, kHomeQuickHeaderOffset, kHomeQuickColumns);
  if (!layout.valid()) return;

  for (int i = 0; i < layout.buttonCount; ++i) {
    const auto tr = layout.tileRect(i);
    Rect tileRect = Rect{tr.x, tr.y, tr.width, tr.height};
    const bool selected = selectedIndex == i;

    // 先绘制选中状态的背景（在图标和文字之前）
    if (selected) {
      const int cornerSize = 12;
      const int lineThickness = 3;
      
      switch (SETTINGS.homeIconStyle) {
        case CrossPointSettings::GRAY_BG_ONLY:
          renderer.fillRectDither(tileRect.x, tileRect.y, tileRect.width, tileRect.height, Color::LightGray);
          break;
          
        case CrossPointSettings::DASHED_BORDER_ONLY:
          for (int i = 0; i < tileRect.width; i += 6) {
            renderer.drawPixel(tileRect.x + i, tileRect.y, true);
            renderer.drawPixel(tileRect.x + i + 1, tileRect.y, true);
            renderer.drawPixel(tileRect.x + i + 2, tileRect.y, true);
            renderer.drawPixel(tileRect.x + i + 3, tileRect.y, true);
            renderer.drawPixel(tileRect.x + i, tileRect.y + tileRect.height - 1, true);
            renderer.drawPixel(tileRect.x + i + 1, tileRect.y + tileRect.height - 1, true);
            renderer.drawPixel(tileRect.x + i + 2, tileRect.y + tileRect.height - 1, true);
            renderer.drawPixel(tileRect.x + i + 3, tileRect.y + tileRect.height - 1, true);
          }
          for (int i = 0; i < tileRect.height; i += 6) {
            renderer.drawPixel(tileRect.x, tileRect.y + i, true);
            renderer.drawPixel(tileRect.x, tileRect.y + i + 1, true);
            renderer.drawPixel(tileRect.x, tileRect.y + i + 2, true);
            renderer.drawPixel(tileRect.x, tileRect.y + i + 3, true);
            renderer.drawPixel(tileRect.x + tileRect.width - 1, tileRect.y + i, true);
            renderer.drawPixel(tileRect.x + tileRect.width - 1, tileRect.y + i + 1, true);
            renderer.drawPixel(tileRect.x + tileRect.width - 1, tileRect.y + i + 2, true);
            renderer.drawPixel(tileRect.x + tileRect.width - 1, tileRect.y + i + 3, true);
          }
          break;
          
        case CrossPointSettings::SOLID_BORDER_ONLY:
          renderer.drawRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, true);
          break;
          
        case CrossPointSettings::CORNERS_AND_GRAY_BG:
          renderer.fillRectDither(tileRect.x, tileRect.y, tileRect.width, tileRect.height, Color::LightGray);
          [[fallthrough]];
          
        case CrossPointSettings::CORNERS_ONLY:
          for (int t = 0; t < lineThickness; t++) {
            renderer.drawLine(tileRect.x, tileRect.y + t, tileRect.x + cornerSize, tileRect.y + t, true);
            renderer.drawLine(tileRect.x + t, tileRect.y, tileRect.x + t, tileRect.y + cornerSize, true);
          }
          for (int t = 0; t < lineThickness; t++) {
            renderer.drawLine(tileRect.x + tileRect.width - cornerSize, tileRect.y + t, tileRect.x + tileRect.width - 1, tileRect.y + t, true);
            renderer.drawLine(tileRect.x + tileRect.width - 1 - t, tileRect.y, tileRect.x + tileRect.width - 1 - t, tileRect.y + cornerSize, true);
          }
          for (int t = 0; t < lineThickness; t++) {
            renderer.drawLine(tileRect.x, tileRect.y + tileRect.height - 1 - t, tileRect.x + cornerSize, tileRect.y + tileRect.height - 1 - t, true);
            renderer.drawLine(tileRect.x + t, tileRect.y + tileRect.height - cornerSize, tileRect.x + t, tileRect.y + tileRect.height - 1, true);
          }
          for (int t = 0; t < lineThickness; t++) {
            renderer.drawLine(tileRect.x + tileRect.width - cornerSize, tileRect.y + tileRect.height - 1 - t,
                              tileRect.x + tileRect.width - 1, tileRect.y + tileRect.height - 1 - t, true);
            renderer.drawLine(tileRect.x + tileRect.width - 1 - t, tileRect.y + tileRect.height - cornerSize,
                              tileRect.x + tileRect.width - 1 - t, tileRect.y + tileRect.height - 1, true);
          }
          break;
      }
    }

    // 计算图标和文字的整体居中位置
    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, label);
    
    const int iconTextGap = 4;
    const int totalContentHeight = mainMenuIconHeight + iconTextGap + lineHeight;
    const int contentStartY = tileRect.y + (tileRect.height - totalContentHeight) / 2 + 4;

    // 绘制图标
    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon);
      const int iconX = tileRect.x + (tileRect.width - mainMenuIconWidth) / 2;
      const int iconY = contentStartY;
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, iconX, iconY, mainMenuIconWidth, mainMenuIconHeight);
      } else if (icon == UIIcon::Apps32) {
        // Keep the Apps glyph distinct from Settings without adding another
        // 72x72 bitmap to the already tight APP1 image.
        constexpr int cell = 14;
        constexpr int gap = 5;
        constexpr int grid = cell * 3 + gap * 2;
        const int gx = iconX + (mainMenuIconWidth - grid) / 2;
        const int gy = iconY + (mainMenuIconHeight - grid) / 2;
        for (int row = 0; row < 3; ++row) {
          for (int col = 0; col < 3; ++col) {
            renderer.fillRect(gx + col * (cell + gap), gy + row * (cell + gap), cell, cell, true);
          }
        }
      }
    }

    // 绘制文字标签（reader face scaled to UI — full CJK menu names）
    const int textX = tileRect.x + (tileRect.width - textWidth) / 2;
    const int textY = contentStartY + mainMenuIconHeight + iconTextGap;
    M4UiText::draw(renderer, UI_10_FONT_ID, textX, textY, label, true);
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

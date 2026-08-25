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
  
  const int fontHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int batteryYOffset = (fontHeight - rect.height) / 2;
  
  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + FengyanMetrics::values.batteryWidth, rect.y,
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
    batteryX -= renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
  }
  drawBattery(renderer,
              Rect{batteryX, rect.y + 6, FengyanMetrics::values.batteryWidth, FengyanMetrics::values.batteryHeight},
              showBatteryPercentage);

  if (title) {
    const int titleMaxWidth = batteryX - rect.x - FengyanMetrics::values.contentSidePadding * 2;
    auto truncatedTitle = M4UiText::truncated(renderer, UI_12_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
    M4UiText::draw(renderer, UI_12_FONT_ID, rect.x + FengyanMetrics::values.contentSidePadding, rect.y + 6,
                   truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
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
      auto subtitle = M4UiText::truncated(renderer, SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      M4UiText::draw(renderer, SMALL_FONT_ID, textX, itemY + subtitleTop, subtitle.c_str(), true);
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
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
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

// 风眼主题核心：三本书籍封面横向排列，中间选中放大
void FengyanTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * FengyanMetrics::values.contentSidePadding) / 3;
  const int tileHeight = FengyanMetrics::values.homeCoverHeight + hPaddingInSelection * 2;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  // 检查是否选中书籍区域（0-2是书籍，3以上是菜单）
  const bool isBookSelected = selectorIndex >= 0 && selectorIndex < std::min(static_cast<int>(recentBooks.size()), FengyanMetrics::values.homeRecentBooksCount);

  // 计算封面区域总宽度和每个封面的宽度（两端对齐）- 在整个函数中使用
  const int totalCoversWidth = tileWidth * 3;
  const int coverSpacing = 8;  // 封面之间的间距
  const int availableCoverWidth = (totalCoversWidth - coverSpacing * 2) / 3;  // 每个封面的可用宽度

  if (hasContinueReading) {
    if (!coverRendered) {
      // 收集所有可用的封面
      std::vector<std::string> availableCovers;
      auto coverDir = SdMan.open("/epub_cover");
      if (coverDir && coverDir.isDirectory()) {
        char fileName[128];
        for (auto coverFile = coverDir.openNextFile(); coverFile; coverFile = coverDir.openNextFile()) {
          if (!coverFile.isDirectory()) {
            coverFile.getName(fileName, sizeof(fileName));
            std::string name(fileName);
            if (name.size() >= 4) {
              std::string ext = name.substr(name.size() - 4);
              for (auto& c : ext) c = tolower(c);
              if (ext == ".bmp") {
                availableCovers.push_back(std::string("/epub_cover/") + fileName);
              }
            }
          }
          coverFile.close();
        }
        coverDir.close();
      }
      
      for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), FengyanMetrics::values.homeRecentBooksCount);
           i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        // 计算每个封面的起始位置（两端对齐）；origin follows rect.x
        int tileX = rect.x + FengyanMetrics::values.contentSidePadding + i * (availableCoverWidth + coverSpacing);
        
        // 判断是否为选中的书籍
        bool isSelected = (selectorIndex == i);
        
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, FengyanMetrics::values.homeCoverWidth, FengyanMetrics::values.homeCoverThumbHeight);
          
          FsFile file;
          if (SdMan.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              // Thumbnail is pre-generated at exact display dimensions (homeCoverWidth x homeCoverThumbHeight)
              // Draw at native bitmap size to avoid any runtime scaling
              const int bmpW = bitmap.getWidth();
              const int bmpH = bitmap.getHeight();
              int displayX = tileX + (availableCoverWidth - bmpW) / 2;
              int displayY = tileY + (tileHeight - bmpH) / 2;
          
              renderer.drawBitmap(bitmap, displayX, displayY, bmpW, bmpH);
          
              // 注意：黑框不在此处绘制，统一在外部绘制以确保跟随选中状态
            } else {
              hasCover = false;
            }
            file.close();
          } else {
            hasCover = false;
          }
        }

        if (!hasCover) {
          // 尝试绘制随机封面或使用默认占位符
          bool randomCoverRendered = false;
          if (!availableCovers.empty()) {
            randomSeed(millis() + i);
            const std::string& randomCoverPath = availableCovers[random(availableCovers.size())];
            FsFile randomFile;
            if (SdMan.openFileForRead("HOME", randomCoverPath, randomFile)) {
              Bitmap bitmap(randomFile);
              if (bitmap.parseHeaders() == BmpReaderError::Ok) {
                // 使用预定义的封面尺寸（随机封面必要时仍需缩放）
                const int displayWidth = FengyanMetrics::values.homeCoverWidth;
                const int displayHeight = FengyanMetrics::values.homeCoverThumbHeight;
                float coverH = static_cast<float>(bitmap.getHeight());
                float coverW = static_cast<float>(bitmap.getWidth());
                float ratio = coverW / coverH;
                const float targetRatio = static_cast<float>(displayWidth) / static_cast<float>(displayHeight);
                float cropX = 1.0f - (targetRatio / ratio);
                if (cropX < 0.0f) cropX = 0.0f;
                int displayX = tileX + (availableCoverWidth - displayWidth) / 2;
                int displayY = tileY + (tileHeight - displayHeight) / 2;

                renderer.drawBitmap(bitmap, displayX, displayY, displayWidth, displayHeight, cropX);

                // 注意：黑框不在此处绘制，统一在外部绘制以确保跟随选中状态
                randomCoverRendered = true;
              }
              randomFile.close();
            }
          }
          if (!randomCoverRendered) {
            // 计算默认占位符尺寸（使用预定义封面尺寸）
            const int displayWidth = FengyanMetrics::values.homeCoverWidth;
            const int displayHeight = FengyanMetrics::values.homeCoverThumbHeight;
            int displayX = tileX + (availableCoverWidth - displayWidth) / 2;
            int displayY = tileY + (tileHeight - displayHeight) / 2;
            
            // 绘制默认占位符：上1/3白底+边框，下2/3深色，类似Lyra主题
            const int topHeight = displayHeight / 3;
            const int bottomHeight = displayHeight - topHeight;
            
            // 上1/3区域：白底+边框
            renderer.drawRect(displayX, displayY, displayWidth, topHeight);
            
            // 下2/3区域：深色填充
            renderer.fillRect(displayX, displayY + topHeight, displayWidth, bottomHeight, true);
            
            // 在上1/3区域居中绘制书本图标
            const int iconSize = 32;
            const int iconX = displayX + (displayWidth - iconSize) / 2;
            const int iconY = displayY + (topHeight - iconSize) / 2;
            renderer.drawIcon(CoverIcon, iconX, iconY, iconSize, iconSize);
          }
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }
    
    // 每次渲染都绘制选中书籍的黑框（不依赖coverRendered，确保移动时跟随）
    if (isBookSelected) {
      const int selectedTileX =
          rect.x + FengyanMetrics::values.contentSidePadding + selectorIndex * (availableCoverWidth + coverSpacing);
      
      // 使用预计算的精确封面尺寸，避免运行时缩放
      const int displayWidth = FengyanMetrics::values.homeCoverWidth;
      const int displayHeight = FengyanMetrics::values.homeCoverThumbHeight;
      int displayX = selectedTileX + (availableCoverWidth - displayWidth) / 2;
      int displayY = tileY + (tileHeight - displayHeight) / 2;
      
      const int borderX = displayX - 4;
      const int borderY = displayY - 4;
      const int borderW = displayWidth + 8;
      const int borderH = displayHeight + 8;
      // 绘制4层实现粗边框效果
      renderer.drawRect(borderX, borderY, borderW, borderH, true);
      renderer.drawRect(borderX + 1, borderY + 1, borderW - 2, borderH - 2, true);
      renderer.drawRect(borderX + 2, borderY + 2, borderW - 4, borderH - 4, true);
      renderer.drawRect(borderX + 3, borderY + 3, borderW - 6, borderH - 6, true);
    }

    // 绘制灰色信息区域（书籍选中或菜单选中时都显示）
    {
      const int infoBoxX = FengyanMetrics::values.contentSidePadding;
#ifdef CROSSPOINT_X3
      const int infoBoxY = tileY + tileHeight + 8;  // X3原位置
#else
      const int infoBoxY = tileY + tileHeight - 4;      // X4往上偏移8像素
#endif
      
      // 动态计算行高
      const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      const int normalLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int padding = 12;  // 上下内边距
      const int lineSpacing = 8;  // 行间距
      
      int infoBoxHeight;
      int currentY = infoBoxY + padding;
      
      if (isBookSelected) {
        // 书籍选中时：显示书籍信息
        const int progressBarHeight = 8;
        
        // 计算总高度
        infoBoxHeight = padding + titleLineHeight + lineSpacing + normalLineHeight + lineSpacing + 
                        normalLineHeight + lineSpacing + progressBarHeight + padding;
        
        // 绘制灰色背景方框（先绘制，三角形后绘制）
        renderer.fillRectDither(infoBoxX, infoBoxY, totalCoversWidth, infoBoxHeight, Color::LightGray);
        renderer.drawRect(infoBoxX, infoBoxY, totalCoversWidth, infoBoxHeight, true);
        
        const auto& selectedBook = recentBooks[selectorIndex];
        const auto meta = M4HomeBookDetailMeta::presentCached(
            selectedBook.path, selectedBook.title, selectedBook.author, selectedBook.progress,
            {L(Str::kValNone), L(Str::kBookSourceLocal), L(Str::kBookSourceUnknown)});

        // 绘制书名行：左侧标题（带虚线下划线）+内容，右侧右对齐
        const std::string& bookTitle = meta.title;
        const char* bookNameLabel = L(Str::kBookTitle);
        M4UiText::draw(renderer, UI_10_FONT_ID, infoBoxX + 12, currentY, bookNameLabel, true);
        int bookNameLabelWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, bookNameLabel);
        // 绘制虚线下划线（4像素实线，2像素间隔）
        for (int x = infoBoxX + 12; x < infoBoxX + 12 + bookNameLabelWidth; x += 6) {
          renderer.drawPixel(x, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 1, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 2, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 3, currentY + normalLineHeight - 2, true);
        }
        // 计算书名可用宽度：总宽度 - 左边距 - 标签宽度 - 最小间距 - 右边距
        const int minGap = 36;
        int titleAvailableWidth = totalCoversWidth - 12 - bookNameLabelWidth - minGap - 12;
        if (titleAvailableWidth < 50) titleAvailableWidth = 50;  // 最小50像素
        auto titleText = M4UiText::truncated(renderer, UI_12_FONT_ID, bookTitle.c_str(), titleAvailableWidth,
                                          EpdFontFamily::BOLD);
        int titleTextWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, titleText.c_str(), EpdFontFamily::BOLD);
        M4UiText::draw(renderer, UI_12_FONT_ID, infoBoxX + totalCoversWidth - 12 - titleTextWidth, currentY,
                       titleText.c_str(), true, EpdFontFamily::BOLD);
        currentY += titleLineHeight + lineSpacing;
        
        // 绘制作者行：左侧标题（带虚线下划线）+内容，右侧右对齐
        const std::string& authorDisplay = meta.author;
        const char* bookAuthorLabel = L(Str::kBookAuthor);
        M4UiText::draw(renderer, SMALL_FONT_ID, infoBoxX + 12, currentY, bookAuthorLabel, true);
        int bookAuthorLabelWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, bookAuthorLabel);
        // 绘制虚线下划线（4像素实线，2像素间隔）
        for (int x = infoBoxX + 12; x < infoBoxX + 12 + bookAuthorLabelWidth; x += 6) {
          renderer.drawPixel(x, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 1, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 2, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 3, currentY + normalLineHeight - 2, true);
        }
        auto authorText = M4UiText::truncated(renderer, SMALL_FONT_ID, authorDisplay.c_str(), totalCoversWidth - 120);
        int authorTextWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, authorText.c_str());
        M4UiText::draw(renderer, SMALL_FONT_ID, infoBoxX + totalCoversWidth - 12 - authorTextWidth, currentY,
                       authorText.c_str(), true);
        currentY += normalLineHeight + lineSpacing;

        // 绘制来源行：插件展示名（或本地/未知来源），不用内部 id
        const std::string& sourceDisplay = meta.source;
        const char* bookSourceLabel = L(Str::kBookSource);
        M4UiText::draw(renderer, UI_10_FONT_ID, infoBoxX + 12, currentY, bookSourceLabel, true);
        int bookSourceLabelWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, bookSourceLabel);
        for (int x = infoBoxX + 12; x < infoBoxX + 12 + bookSourceLabelWidth; x += 6) {
          renderer.drawPixel(x, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 1, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 2, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 3, currentY + normalLineHeight - 2, true);
        }
        auto sourceText = M4UiText::truncated(renderer, UI_10_FONT_ID, sourceDisplay.c_str(), totalCoversWidth - 120);
        int sourceTextWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, sourceText.c_str());
        M4UiText::draw(renderer, UI_10_FONT_ID, infoBoxX + totalCoversWidth - 12 - sourceTextWidth, currentY,
                       sourceText.c_str(), true);
        currentY += normalLineHeight + lineSpacing;

        // 阅读进度只由图形进度条表示；计算语义仍用 recentBooks[].progress
        int bookProgress = recentBooks[selectorIndex].progress;
        
        // 绘制进度条背景
        renderer.fillRect(infoBoxX + 12, currentY, totalCoversWidth - 24, progressBarHeight, false);
        renderer.drawRect(infoBoxX + 12, currentY, totalCoversWidth - 24, progressBarHeight, true);
        
        // 绘制进度条填充
        if (bookProgress > 0) {
          int fillWidth = ((totalCoversWidth - 28) * bookProgress) / 100;
          renderer.fillRect(infoBoxX + 14, currentY + 2, fillWidth, progressBarHeight - 4, true);
        }
        
        // 绘制指向选中书籍的三角形（朝上，黑色边框+浅灰填充）
        // 计算选中书籍的位置和黑框底部
        const int selectedTileX =
          rect.x + FengyanMetrics::values.contentSidePadding + selectorIndex * (availableCoverWidth + coverSpacing);
        const int displayWidth = FengyanMetrics::values.homeCoverWidth;
        const int displayHeight = FengyanMetrics::values.homeCoverThumbHeight;
        int displayX = selectedTileX + (availableCoverWidth - displayWidth) / 2;
        int displayY = tileY + (tileHeight - displayHeight) / 2;
        const int borderBottomY = displayY + displayHeight + 4;  // 黑框底部位置（考虑4px边框）
        
        const int triangleCenterX = displayX + displayWidth / 2;
        const int triangleSize = 14;  // 三角形尺寸
        // 三角形向下延伸2像素进入灰色区域，让底边"覆盖"灰框上边线
        const int triangleTopY = borderBottomY + 1;
        
        // 绘制实心三角形（朝上）- 使用与灰框相同的 fillRectDither 填充
        for (int row = 1; row < triangleSize; row++) {
          int lineWidth = (row + 1) * 2 - 1 - 2;  // 内部宽度（减去边框）
          if (lineWidth < 1) lineWidth = 1;
          int startX = triangleCenterX - lineWidth / 2;
          int y = triangleTopY + row;
          // 使用 fillRectDither 绘制内部填充，与灰框颜色一致
          renderer.fillRectDither(startX, y, lineWidth, 1, Color::LightGray);
        }
        
        // 绘制三角形边框（外轮廓）- 左右两边用黑色，底边用浅灰色与灰框融合
        // 左边框
        for (int row = 0; row < triangleSize; row++) {
          int lineWidth = (row + 1) * 2 - 1;
          int leftX = triangleCenterX - lineWidth / 2;
          int rightX = triangleCenterX + lineWidth / 2;
          int y = triangleTopY + row;
          renderer.drawPixel(leftX, y, true);   // 左边框（黑色）
          renderer.drawPixel(rightX, y, true);  // 右边框（黑色）
        }
        // 底边框（延伸进入灰框内部）- 使用浅灰色，与灰框融为一体
        int bottomY = triangleTopY + triangleSize - 1;
        int bottomWidth = triangleSize * 2 - 1;
        int bottomStartX = triangleCenterX - bottomWidth / 2;
        renderer.fillRectDither(bottomStartX, bottomY, bottomWidth, 1, Color::LightGray);
        // 顶顶点
        renderer.drawPixel(triangleCenterX, triangleTopY, true);
      } else {
        // 菜单选中时：显示阅读统计
        infoBoxHeight = padding + titleLineHeight + lineSpacing + normalLineHeight + lineSpacing + normalLineHeight + padding;
        
        // 绘制灰色背景方框
        renderer.fillRectDither(infoBoxX, infoBoxY, totalCoversWidth, infoBoxHeight, Color::LightGray);
        renderer.drawRect(infoBoxX, infoBoxY, totalCoversWidth, infoBoxHeight, true);
        
        // 绘制标题"阅读统计"（去掉虚线下划线）
        const char* titleText = L(Str::kReadingStats);
        M4UiText::draw(renderer, UI_12_FONT_ID, infoBoxX + 12, currentY, titleText, true, EpdFontFamily::BOLD);
        currentY += titleLineHeight + lineSpacing;
        
        // 左侧固定起始位置
        const int leftLabelX = infoBoxX + 12;
        // 右侧固定结束位置（距离右边缘24像素，增加间距）
        const int rightValueX = infoBoxX + totalCoversWidth - 24;
        // 中间安全间距区域，确保左右文字不会挨在一起
        const int minGap = 36;  // 最小间距20像素
        
        // 累计阅读时长 - 左侧标题（带虚线下划线，无冒号），右侧值右对齐
        const char* totalLabel = L(Str::kTotalReadingTime);
        std::string totalValueStr = ReadingStatsStore::formatReadingTime(READING_STATS.getTotalReadingTime());
        const char* totalValue = totalValueStr.c_str();
        int totalLabelWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, totalLabel);
        int totalValueWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, totalValue);
        M4UiText::draw(renderer, UI_10_FONT_ID, leftLabelX, currentY, totalLabel, true);
        // 绘制虚线下划线（4像素实线，2像素间隔）
        for (int x = leftLabelX; x < leftLabelX + totalLabelWidth; x += 6) {
          renderer.drawPixel(x, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 1, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 2, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 3, currentY + normalLineHeight - 2, true);
        }
        // 计算右侧值的位置，确保与左侧标签至少有minGap的间距
        int totalValueX = rightValueX - totalValueWidth;
        if (totalValueX < leftLabelX + totalLabelWidth + minGap) {
          totalValueX = leftLabelX + totalLabelWidth + minGap;
        }
        M4UiText::draw(renderer, UI_10_FONT_ID, totalValueX, currentY, totalValue, true);
        currentY += normalLineHeight + lineSpacing;
        
        // 上次阅读时长 - 左侧标题（带虚线下划线，无冒号），右侧值右对齐
        const char* sessionLabel = L(Str::kLastReadingTime);
        std::string sessionValueStr = ReadingStatsStore::formatReadingTime(READING_STATS.getSessionReadingTime());
        const char* sessionValue = sessionValueStr.c_str();
        int sessionLabelWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, sessionLabel);
        int sessionValueWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, sessionValue);
        M4UiText::draw(renderer, UI_10_FONT_ID, leftLabelX, currentY, sessionLabel, true);
        // 绘制虚线下划线（4像素实线，2像素间隔）
        for (int x = leftLabelX; x < leftLabelX + sessionLabelWidth; x += 6) {
          renderer.drawPixel(x, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 1, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 2, currentY + normalLineHeight - 2, true);
          renderer.drawPixel(x + 3, currentY + normalLineHeight - 2, true);
        }
        // 计算右侧值的位置，确保与左侧标签至少有minGap的间距
        int sessionValueX = rightValueX - sessionValueWidth;
        if (sessionValueX < leftLabelX + sessionLabelWidth + minGap) {
          sessionValueX = leftLabelX + sessionLabelWidth + minGap;
        }
        M4UiText::draw(renderer, UI_10_FONT_ID, sessionValueX, currentY, sessionValue, true);
      }
    }
  } else {
    // 没有阅读记录时，绘制浅灰色背景的居中提示框
    const int boxPadding = FengyanMetrics::values.contentSidePadding;
    const int boxX = boxPadding;
    const int boxY = rect.y + hPaddingInSelection;
    const int boxW = rect.width - 2 * boxPadding;
    const int boxH = rect.height - hPaddingInSelection - 10;

    renderer.fillRectDither(boxX, boxY, boxW, boxH, Color::LightGray);
    
    // 绘制虚线边框
    constexpr int dashOn = 6, dashOff = 4, dashCycle = dashOn + dashOff;
    for (int i = 0; i < boxW; i++) {
      if (i % dashCycle < dashOn) {
        renderer.drawPixel(boxX + i, boxY, true);
        renderer.drawPixel(boxX + i, boxY + boxH - 1, true);
      }
    }
    for (int i = 0; i < boxH; i++) {
      if (i % dashCycle < dashOn) {
        renderer.drawPixel(boxX, boxY + i, true);
        renderer.drawPixel(boxX + boxW - 1, boxY + i, true);
      }
    }

    const int textW = M4UiText::textWidth(renderer, UI_10_FONT_ID, L(Str::kNoReadingHistory));
    const int textH = renderer.getLineHeight(UI_10_FONT_ID);
    const int textX = boxX + (boxW - textW) / 2;
    const int textY = boxY + (boxH - textH) / 2;
    M4UiText::draw(renderer, UI_10_FONT_ID, textX, textY, L(Str::kNoReadingHistory), true);
  }
}

// 风眼主题核心：底部6个功能图标网格布局（2行3列）
void FengyanTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  // Shared layout with TouchHitGeometry::makeFengyanMenuLayout (3 cols, rows=ceil(n/3)).
  const auto layout = TouchHitGeometry::makeFengyanMenuLayout(
      TouchHitGeometry::Rect{rect.x, rect.y, rect.width, rect.height}, buttonCount,
      FengyanMetrics::values.contentSidePadding, -6, 3);
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

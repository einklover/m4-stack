#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <HalPowerManager.h>
#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
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
#include "components/icons/cog.h"
#include "CrossPointSettings.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/StringUtils.h"

// Internal constants
namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int listIconSize = 24;
constexpr int mainMenuIconSize = 32;

const uint8_t* iconForName(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:   return Folder24Icon;
    case UIIcon::Book:     return Book24Icon;
    case UIIcon::Text:     return Text24Icon;
    case UIIcon::Image:    return Image24Icon;
    case UIIcon::File:     return File24Icon;
    case UIIcon::Library:  return LibraryIcon;
    case UIIcon::Recent:   return RecentIcon;
    case UIIcon::Settings: return SettingsIcon;
    case UIIcon::Transfer: return TransferIcon;
    case UIIcon::Wifi:     return WifiIcon;
    case UIIcon::Hotspot:  return HotspotIcon;
    case UIIcon::Cog:      return CogIcon;
    default:               return nullptr;
  }
}

const uint8_t* iconForName(UIIcon icon, int /*size*/) {
  return iconForName(icon);
}
}  // namespace

void LyraTheme::drawBattery(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned battery icon和百分比
  const uint16_t percentage = powerManager.getBatteryPercentage();
  
  // 获取小字体的高度，用于垂直居中计算
  const int fontHeight = renderer.getLineHeight(SMALL_FONT_ID);
  // 计算垂直居中偏移：让电池和文字在同一基线上对齐
  // 文字绘制在 rect.y 位置，电池应该居中于文字
  const int batteryYOffset = (fontHeight - rect.height) / 2;
  
  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + LyraMetrics::values.batteryWidth, rect.y,
                      percentageText.c_str());
  }

  const int x = rect.x;
  const int y = rect.y + batteryYOffset;
  const int battWidth = LyraMetrics::values.batteryWidth - 4;  // 留出正极凸起的空间
  const int battHeight = rect.height;

  // 绘制电池外框（方正矩形）
  // 上边
  renderer.drawLine(x, y, x + battWidth - 1, y);
  // 下边
  renderer.drawLine(x, y + battHeight - 1, x + battWidth - 1, y + battHeight - 1);
  // 左边
  renderer.drawLine(x, y, x, y + battHeight - 1);
  // 右边（电池主体右侧）
  renderer.drawLine(x + battWidth - 1, y, x + battWidth - 1, y + battHeight - 1);

  // 电池正极凸起（右侧，方正样式）
  const int terminalX = x + battWidth;
  const int terminalHeight = battHeight / 2;
  const int terminalY = y + (battHeight - terminalHeight) / 2;
  renderer.fillRect(terminalX, terminalY, 2, terminalHeight);

  // 内边距
  const int innerX = x + 2;
  const int innerY = y + 2;
  const int innerWidth = battWidth - 4;
  const int innerHeight = battHeight - 4;

  // 绘制4段电池电量条（每段代表25%）
  // 每段宽度 = (innerWidth - 3个间隙) / 4
  const int segmentWidth = (innerWidth - 3) / 4;
  const int gap = 1;

  // 根据电量百分比决定显示几段
  int segmentsToShow = 0;
  if (percentage > 0) segmentsToShow = 1;
  if (percentage > 25) segmentsToShow = 2;
  if (percentage > 50) segmentsToShow = 3;
  if (percentage > 75) segmentsToShow = 4;

  for (int i = 0; i < segmentsToShow; i++) {
    renderer.fillRect(innerX + i * (segmentWidth + gap), innerY, segmentWidth, innerHeight);
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  int batteryX = rect.x + rect.width - LyraMetrics::values.contentSidePadding - LyraMetrics::values.batteryWidth;
  if (showBatteryPercentage) {
    const uint16_t percentage = powerManager.getBatteryPercentage();
    const auto percentageText = std::to_string(percentage) + "%";
    batteryX -= renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
  }
  drawBattery(renderer,
              Rect{batteryX, rect.y + 6, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
              showBatteryPercentage);

  if (title) {
    // 标题显示在状态栏左侧，宽度不超过电池图标左边界
    const int titleMaxWidth = batteryX - rect.x - LyraMetrics::values.contentSidePadding * 2;
    auto truncatedTitle = M4UiText::truncated(renderer, UI_12_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
    M4UiText::draw(renderer, UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding, rect.y + 6,
                   truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width, rect.y + rect.height - 3, 1, true);
  }
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

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

    currentX += textWidth + LyraMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width, rect.y + rect.height - 1, true);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue) const {
  const bool hasSubtitle = rowSubtitle != nullptr;
  int rowHeight = M4UiText::listRowHeight(
      renderer, UI_10_FONT_ID,
      hasSubtitle ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight,
      hasSubtitle);
  int pageItems = std::max(1, rowHeight > 0 ? rect.height / rowHeight : 1);

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  // Draw selection
  int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(LyraMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - LyraMetrics::values.contentSidePadding * 2, rowHeight, cornerRadius,
                             Color::LightGray);
  }

  // Calculate text position considering icon
  int textX = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  if (rowIcon != nullptr) {
    textX += listIconSize + hPaddingInSelection;
    textWidth -= listIconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  // Align to the title/subtitle block, not merely to the touch row. This
  // keeps title-only and two-line rows on the same visual baseline.
  const int subtitleTop = M4UiText::listSubtitleTop(renderer, UI_10_FONT_ID);
  const int iconYOffset = M4UiText::listIconTop(renderer, UI_10_FONT_ID, rowHeight,
                                                hasSubtitle, listIconSize, 4, subtitleTop);
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    // Draw icon if provided
    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection,
                          itemY + iconYOffset, listIconSize, listIconSize);
      }
    }

    // Draw name (reader face scaled to UI metrics — full CJK coverage)
    int rowTextWidth = textWidth - (rowValue != nullptr ? 60 : 0);
    auto itemName = rowTitle(i);
    auto item = M4UiText::truncated(renderer, UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    M4UiText::draw(renderer, UI_10_FONT_ID, textX, itemY + 4, item.c_str(), true);

    if (hasSubtitle) {
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = M4UiText::truncated(renderer, SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      M4UiText::draw(renderer, SMALL_FONT_ID, textX, itemY + subtitleTop, subtitle.c_str(), true);
    }

    if (rowValue != nullptr) {
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        constexpr int maxValueWidth = 240;
        auto truncatedValue = M4UiText::truncated(renderer, UI_10_FONT_ID, valueText.c_str(), maxValueWidth);
        const auto valueTextWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, truncatedValue.c_str());

        if (i == selectedIndex) {
          renderer.fillRoundedRect(
              contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection * 2 - valueTextWidth, itemY,
              valueTextWidth + hPaddingInSelection * 2, rowHeight, cornerRadius, Color::Black);
        }

        const bool isWhiteText = (i == selectedIndex);
        const bool isGrayArrow = (truncatedValue == ">" && !isWhiteText);
        
        if (isGrayArrow) {
          M4UiText::draw(renderer, UI_10_FONT_ID,
                         contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection - valueTextWidth,
                         itemY + 4, truncatedValue.c_str(), false);
        } else {
          M4UiText::draw(renderer, UI_10_FONT_ID,
                         contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection - valueTextWidth,
                         itemY + 4, truncatedValue.c_str(), !isWhiteText);
        }
      }
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4, bool force) const {
  if (!force && !SETTINGS.buttonHintsEnabled) return;

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  constexpr int buttonPositions[] = {38, 154, 268, 384};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
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

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                                    bool force) const {
  if (!force && !SETTINGS.buttonHintsEnabled) return;

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  // X3 layout: Up on left side, Down on right side, positioned higher
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

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * LyraMetrics::values.contentSidePadding) / 3;
  const int tileHeight = rect.height;
  const int bookTitleHeight = tileHeight - LyraMetrics::values.homeCoverHeight - hPaddingInSelection;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      // Collect all available cover BMP paths from /epub_cover directory
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

      for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), LyraMetrics::values.homeRecentBooksCount);
           i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = LyraMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, LyraMetrics::values.homeCoverWidth, LyraMetrics::values.homeCoverThumbHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (SdMan.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              // Thumbnail is pre-generated at exact display dimensions
              // Draw at native bitmap size to avoid any runtime scaling
              const int bmpW = bitmap.getWidth();
              const int bmpH = bitmap.getHeight();
              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  bmpW, bmpH);
            } else {
              // BMP 解析失败，回落至随机封面
              hasCover = false;
            }
            file.close();
          } else {
            // 缩略图文件不存在或无法打开，回落至随机封面
            hasCover = false;
          }
        }

        if (!hasCover) {
          // Try to draw a random cover from available covers on SD card
          bool randomCoverRendered = false;
          if (!availableCovers.empty()) {
            randomSeed(millis() + i);
            const std::string& randomCoverPath = availableCovers[random(availableCovers.size())];
            FsFile randomFile;
            if (SdMan.openFileForRead("HOME", randomCoverPath, randomFile)) {
              Bitmap bitmap(randomFile);
              if (bitmap.parseHeaders() == BmpReaderError::Ok) {
                // 随机封面使用预定义尺寸进行缩放
                const int dispW = LyraMetrics::values.homeCoverWidth;
                const int dispH = LyraMetrics::values.homeCoverThumbHeight;
                float coverH = static_cast<float>(bitmap.getHeight());
                float coverW = static_cast<float>(bitmap.getWidth());
                float ratio = coverW / coverH;
                const float targetRatio = static_cast<float>(dispW) / static_cast<float>(dispH);
                float cropX = 1.0f - (targetRatio / ratio);
                if (cropX < 0.0f) cropX = 0.0f;
                renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                    dispW, dispH, cropX);
                randomCoverRendered = true;
              }
              randomFile.close();
            }
          }
          if (!randomCoverRendered) {
            const int coverW = LyraMetrics::values.homeCoverWidth;
            const int coverH = LyraMetrics::values.homeCoverThumbHeight;
            // 上 1/3 区域边框
            renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                              coverW, coverH / 3);
            // 下 2/3 填充深色作为默认封面背景
            renderer.fillRect(tileX + hPaddingInSelection,
                              tileY + hPaddingInSelection + coverH / 3,
                              coverW, 2 * coverH / 3, true);
            // 在上 1/3 区域居中绘制书本图标
            const int iconX = tileX + hPaddingInSelection + (coverW - 32) / 2;
            const int iconY = tileY + hPaddingInSelection + (coverH / 3 - 32) / 2;
            renderer.drawIcon(CoverIcon, iconX, iconY, 32, 32);
          }
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), LyraMetrics::values.homeRecentBooksCount); i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = LyraMetrics::values.contentSidePadding + tileWidth * i;
      const int textW = tileWidth - 2 * hPaddingInSelection;
      auto title =
          M4UiText::truncated(renderer, SMALL_FONT_ID, recentBooks[i].title.c_str(), textW);
      const char* authorSrc = recentBooks[i].author.c_str();
      auto author = M4UiText::truncated(renderer, SMALL_FONT_ID,
                                        (authorSrc && authorSrc[0]) ? authorSrc : " ", textW);

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                                LyraMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, LyraMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + LyraMetrics::values.homeCoverHeight + hPaddingInSelection, tileWidth,
                                 bookTitleHeight, cornerRadius, false, false, true, true, Color::LightGray);
      }
      const int textY = tileY + tileHeight - bookTitleHeight + hPaddingInSelection + 2;
      M4UiText::draw(renderer, SMALL_FONT_ID, tileX + hPaddingInSelection, textY, title.c_str(), true);
      if (!recentBooks[i].author.empty()) {
        M4UiText::draw(renderer, SMALL_FONT_ID, tileX + hPaddingInSelection,
                       textY + M4UiText::listLineHeight(renderer, SMALL_FONT_ID) + 2, author.c_str(), true);
      }
    }
  } else {
    // 没有阅读记录时，绘制浅灰色背景的居中提示框
    const int boxPadding = LyraMetrics::values.contentSidePadding;
    const int boxX = boxPadding;
    const int boxY = rect.y + hPaddingInSelection;
    const int boxW = rect.width - 2 * boxPadding;
    // 缩小框高度，使与下方菜单间距约 20px
    const int boxH = rect.height - hPaddingInSelection - 10;

    renderer.fillRectDither(boxX, boxY, boxW, boxH, Color::LightGray);
    // 绘制虚线边框（6px 画 + 4px 空）
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

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = (rect.width - LyraMetrics::values.contentSidePadding * 2 - LyraMetrics::values.menuSpacing) / 2;
    Rect tileRect =
        Rect{rect.x + LyraMetrics::values.contentSidePadding + (LyraMetrics::values.menuSpacing + tileWidth) * (i % 2),
             rect.y + static_cast<int>(i / 2) * (LyraMetrics::values.menuRowHeight + LyraMetrics::values.menuSpacing),
             tileWidth, LyraMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (LyraMetrics::values.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon);
      if (iconBitmap != nullptr) {
        // Align icon Y with text baseline using same offset as crosspoint-reader reference
        renderer.drawIcon(iconBitmap, textX, textY + 3, mainMenuIconSize, mainMenuIconSize);
        textX += mainMenuIconSize + hPaddingInSelection;
      }
    }

    M4UiText::draw(renderer, UI_12_FONT_ID, textX, textY + 2, label, true);
  }
}

Rect LyraTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
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

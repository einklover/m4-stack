#include "UITheme.h"

#include <GfxRenderer.h>

#include <memory>

#include "RecentBooksStore.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/fengyan/FengyanTheme.h"
#include "util/StringUtils.h"

UITheme UITheme::instance;

UITheme::UITheme() {
  // 默认使用风眼主题
  Serial.printf("[%lu] [UI] Using Fengyan theme\n", millis());
  currentTheme = std::make_unique<FengyanTheme>();
  currentMetrics = &FengyanMetrics::values;
  currentThemeType = ThemeType::Fengyan;
}

void UITheme::reload() {
  // Re-apply Fengyan theme. unique_ptr assignment frees the previous instance
  // (fixes leak from repeated new without delete on settings exit / boot).
  currentTheme = std::make_unique<FengyanTheme>();
  currentMetrics = &FengyanMetrics::values;
  currentThemeType = ThemeType::Fengyan;
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverWidth, int coverHeight) {
  // Replace [WIDTH] placeholder first
  size_t posW = coverBmpPath.find("[WIDTH]", 0);
  if (posW != std::string::npos) {
    coverBmpPath.replace(posW, 7, std::to_string(coverWidth));
  }
  // Replace [HEIGHT] placeholder
  size_t posH = coverBmpPath.find("[HEIGHT]", 0);
  if (posH != std::string::npos) {
    coverBmpPath.replace(posH, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  // Check if it's a directory (ends with '/')
  if (!filename.empty() && filename.back() == '/') {
    return UIIcon::Folder;
  }
  // Check for book file extensions
  if (StringUtils::checkFileExtension(filename, ".epub") ||
      StringUtils::checkFileExtension(filename, ".xtc") ||
      StringUtils::checkFileExtension(filename, ".xtch")) {
    return UIIcon::Book;
  }
  // Check for text file extensions
  if (StringUtils::checkFileExtension(filename, ".txt") ||
      StringUtils::checkFileExtension(filename, ".md")) {
    return UIIcon::Text;
  }
  // Check for image file extensions
  if (StringUtils::checkFileExtension(filename, ".bmp") ||
      StringUtils::checkFileExtension(filename, ".png") ||
      StringUtils::checkFileExtension(filename, ".jpg") ||
      StringUtils::checkFileExtension(filename, ".jpeg")) {
    return UIIcon::Image;
  }
  // Check for font file extensions
  if (StringUtils::checkFileExtension(filename, ".epdfont")) {
    return UIIcon::Text;  // 使用文本图标表示字体文件
  }
  // Default to generic file icon
  return UIIcon::File;
}

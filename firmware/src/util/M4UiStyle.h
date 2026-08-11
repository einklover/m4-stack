#pragma once

// Lightweight, renderer-independent UI style and geometry contract.
// All theme/layout objects are plain value types (no vectors, maps or owning
// pointers) so a scene can keep one Theme without internal-heap churn.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace M4UiStyle {

enum class Ink : uint8_t { White = 0, Black = 1 };

struct Palette {
  Ink background = Ink::White;
  Ink foreground = Ink::Black;
  Ink divider = Ink::Black;
  Ink selectionBackground = Ink::Black;
  Ink selectionForeground = Ink::White;
  Ink disabled = Ink::Black;  // monochrome renderer uses stipple/dither
};

struct TextStyle {
  int layoutFontId = 0;
  int lineHeight = 16;
  bool bold = false;
};

struct Typography {
  TextStyle title{};
  TextStyle row{};
  TextStyle subtitle{};
  TextStyle button{};
  TextStyle caption{};
};

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  bool valid() const { return width > 0 && height > 0; }
  bool contains(int px, int py) const {
    return valid() && px >= x && px < x + width && py >= y && py < y + height;
  }
};

struct SystemMetrics {
  // Values from UITheme::getMetrics(), expressed at the reference panel size.
  int topPadding = 5;
  int batteryBarHeight = 40;
  int headerHeight = 44;
  int verticalSpacing = 16;
  int contentSidePadding = 20;
  int listRowHeight = 52;
  int listWithSubtitleRowHeight = 72;
  int buttonHintsHeight = 40;
};

struct FontRoles {
  int title = 0;
  int row = 0;
  int subtitle = 0;
  int button = 0;
  int caption = 0;
};

struct ButtonStyle {
  int minTouchWidth = 48;
  int minTouchHeight = 48;
  int horizontalPadding = 12;
  int verticalPadding = 8;
  // Reserve a real center slot for the page indicator.  The previous 8px
  // gap let "1/2" overlap both button borders on the 480px touch panel.
  int gap = 56;
  int borderWidth = 1;
};

struct ListStyle {
  int rowHeight = 72;
  int subtitleRowHeight = 72;
  int titleInsetX = 12;
  int subtitleGap = 4;
  int dividerInsetX = 0;
  int visibleRows = 1;
};

struct Metrics {
  int screenWidth = 480;
  int screenHeight = 800;
  int contentPadding = 20;
  Rect header{};
  Rect content{};
  Rect footer{};
};

struct Theme {
  Metrics metrics{};
  Palette colors{};
  Typography type{};
  ButtonStyle button{};
  ListStyle list{};
};

inline int scaleRound(int value, int extent, int referenceExtent) {
  if (value <= 0 || extent <= 0 || referenceExtent <= 0) return 0;
  return static_cast<int>((static_cast<int64_t>(value) * extent + referenceExtent / 2) /
                          referenceExtent);
}

inline Theme makeTheme(int screenWidth, int screenHeight, const SystemMetrics& system,
                       const FontRoles& fonts = {}) {
  Theme t;
  t.metrics.screenWidth = std::max(1, screenWidth);
  t.metrics.screenHeight = std::max(1, screenHeight);
  const auto sx = [&](int value) { return std::max(1, scaleRound(value, t.metrics.screenWidth, 480)); };
  const auto sy = [&](int value) { return std::max(1, scaleRound(value, t.metrics.screenHeight, 800)); };

  t.metrics.contentPadding = sx(std::max(1, system.contentSidePadding));
  // UITheme::drawHeader() already owns the complete top/status bar, including
  // the battery area.  batteryBarHeight is descriptive theme metadata, not an
  // additional vertical band to reserve here.  Counting it a second time
  // pushed the scene status line to ~105px and made it collide visually with
  // the first list row.  Keep the same contract as native activities:
  // topPadding + headerHeight + one vertical spacing gutter.
  const int systemHeader = system.topPadding + system.headerHeight + system.verticalSpacing;
  const int headerHeight = sy(std::max(64, systemHeader));
  const int footerHeight = sy(std::max(48, system.buttonHintsHeight));
  t.metrics.header = {0, 0, t.metrics.screenWidth, headerHeight};
  t.metrics.footer = {0, std::max(headerHeight, t.metrics.screenHeight - footerHeight),
                      t.metrics.screenWidth, footerHeight};
  const int contentHeight = std::max(0, t.metrics.footer.y - t.metrics.header.height);
  t.metrics.content = {t.metrics.contentPadding, t.metrics.header.height,
                       std::max(0, t.metrics.screenWidth - 2 * t.metrics.contentPadding), contentHeight};

  t.button.minTouchWidth = sx(48);
  t.button.minTouchHeight = sy(48);
  t.button.horizontalPadding = sx(12);
  t.button.verticalPadding = sy(8);
  t.button.gap = sx(56);
  // A plain list row is the common host-scene case.  Keep the subtitle row
  // separately so callers do not silently inflate every list to the larger
  // two-line height; both values still come from the active system theme.
  t.list.rowHeight = sy(std::max(1, system.listRowHeight));
  t.list.subtitleRowHeight = sy(std::max(system.listRowHeight, system.listWithSubtitleRowHeight));
  t.list.titleInsetX = sx(12);
  t.list.subtitleGap = sy(4);
  t.list.visibleRows = contentHeight > 0
                           ? std::max(1, contentHeight / std::max(1, t.list.rowHeight))
                           : 0;

  t.type.title = {fonts.title, sy(24), true};
  t.type.row = {fonts.row, sy(22), false};
  t.type.subtitle = {fonts.subtitle, sy(18), false};
  t.type.button = {fonts.button, sy(20), false};
  t.type.caption = {fonts.caption, sy(16), false};
  return t;
}

inline Rect rowRect(const Theme& theme, int visibleIndex0) {
  if (visibleIndex0 < 0 || visibleIndex0 >= theme.list.visibleRows) return {};
  return {theme.metrics.content.x,
          theme.metrics.content.y + visibleIndex0 * theme.list.rowHeight,
          theme.metrics.content.width,
          std::min(theme.list.rowHeight,
                   theme.metrics.content.y + theme.metrics.content.height -
                       (theme.metrics.content.y + visibleIndex0 * theme.list.rowHeight))};
}

inline int rowAt(const Theme& theme, int x, int y, int visibleCount) {
  if (!theme.metrics.content.contains(x, y) || theme.list.rowHeight <= 0) return -1;
  const int row = (y - theme.metrics.content.y) / theme.list.rowHeight;
  const int limit = std::min(std::max(0, visibleCount), theme.list.visibleRows);
  return row >= 0 && row < limit ? row : -1;
}

inline Rect expandHitbox(Rect visual, int minWidth, int minHeight, int screenWidth, int screenHeight) {
  if (!visual.valid()) return {};
  const int targetW = std::max(visual.width, minWidth);
  const int targetH = std::max(visual.height, minHeight);
  Rect hit{visual.x - (targetW - visual.width) / 2, visual.y - (targetH - visual.height) / 2,
           targetW, targetH};
  if (hit.x < 0) hit.x = 0;
  if (hit.y < 0) hit.y = 0;
  if (hit.x + hit.width > screenWidth) hit.x = std::max(0, screenWidth - hit.width);
  if (hit.y + hit.height > screenHeight) hit.y = std::max(0, screenHeight - hit.height);
  hit.width = std::min(hit.width, screenWidth);
  hit.height = std::min(hit.height, screenHeight);
  return hit;
}

inline Rect footerButtonRect(const Theme& theme, int index0, int buttonCount) {
  if (buttonCount <= 0 || index0 < 0 || index0 >= buttonCount) return {};
  const Rect footer = theme.metrics.footer;
  const int totalGap = theme.button.gap * (buttonCount - 1);
  const int available = std::max(0, footer.width - 2 * theme.metrics.contentPadding - totalGap);
  const int width = buttonCount > 0 ? available / buttonCount : 0;
  return {theme.metrics.contentPadding + index0 * (width + theme.button.gap), footer.y, width,
          footer.height};
}

// The page indicator is a footer element, not a button label.  Keep its
// drawing slot explicit so host rendering/tests can prove it never intersects
// either touch target.
inline Rect footerPageSlot(const Theme& theme, int buttonCount = 2) {
  if (buttonCount < 2) return {};
  const Rect left = footerButtonRect(theme, 0, buttonCount);
  const Rect right = footerButtonRect(theme, 1, buttonCount);
  const int x = left.x + left.width;
  const int width = right.x - x;
  if (width <= 0) return {};
  return {x, theme.metrics.footer.y, width, theme.metrics.footer.height};
}

inline int footerButtonAt(const Theme& theme, int x, int y, int buttonCount) {
  for (int i = 0; i < buttonCount; ++i) {
    const Rect hit = expandHitbox(footerButtonRect(theme, i, buttonCount), theme.button.minTouchWidth,
                                  theme.button.minTouchHeight, theme.metrics.screenWidth,
                                  theme.metrics.screenHeight);
    if (hit.contains(x, y)) return i;
  }
  return -1;
}

using GlyphAdvanceFn = int (*)(const char* utf8, size_t bytes, void* context);

inline size_t utf8SequenceBytes(unsigned char lead) {
  if ((lead & 0x80u) == 0) return 1;
  if ((lead & 0xE0u) == 0xC0u) return 2;
  if ((lead & 0xF0u) == 0xE0u) return 3;
  if ((lead & 0xF8u) == 0xF0u) return 4;
  return 1;
}

// UTF-8-safe, constant-scratch title elision. The returned string is bounded
// by maxWidth; Theme itself remains allocation-free.
inline std::string ellipsizeUtf8(const char* text, int maxWidth, GlyphAdvanceFn advance,
                                 void* context = nullptr, const char* ellipsis = "…") {
  if (!text || !text[0] || maxWidth <= 0 || !advance) return {};
  const size_t ellipsisBytes = std::strlen(ellipsis);
  const int ellipsisWidth = std::max(0, advance(ellipsis, ellipsisBytes, context));
  if (ellipsisWidth > maxWidth) return {};

  const size_t total = std::strlen(text);
  int fullWidth = 0;
  for (size_t i = 0; i < total;) {
    size_t bytes = utf8SequenceBytes(static_cast<unsigned char>(text[i]));
    if (i + bytes > total) bytes = 1;
    fullWidth += std::max(0, advance(text + i, bytes, context));
    i += bytes;
  }
  if (fullWidth <= maxWidth) return text;

  std::string out;
  int width = 0;
  for (size_t i = 0; i < total;) {
    size_t bytes = utf8SequenceBytes(static_cast<unsigned char>(text[i]));
    if (i + bytes > total) bytes = 1;
    const int glyph = std::max(0, advance(text + i, bytes, context));
    if (width + glyph + ellipsisWidth > maxWidth) {
      out += ellipsis;
      return out;
    }
    out.append(text + i, bytes);
    width += glyph;
    i += bytes;
  }
  return out;
}

}  // namespace M4UiStyle

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace M4NativeUi {

inline constexpr int kSchemaVersion = 1;

struct Limits {
  size_t maxBytes = 32u * 1024u;
  size_t maxScreens = 12;
  size_t maxNodesPerScreen = 48;
  size_t maxAttrBytes = 512;
  size_t maxStringBytes = 256;
};

enum class NodeType : uint8_t {
  Text = 0,
  FlowText,
  Image,
  List,
  Tiles,
  Tabs,
  Progress,
  Spacer,
  Divider,
  Buttons,
};

// CSS-like utility classes, compiled to flags while parsing. This deliberately
// avoids selectors, cascading, inheritance, expressions and arbitrary numeric
// style properties. XML stays pleasant to author while runtime cost is fixed.
enum StyleFlag : uint16_t {
  StyleNone = 0,
  StyleCompact = 1u << 0,
  StyleHero = 1u << 1,
  StyleSection = 1u << 2,
  StyleMuted = 1u << 3,
  StyleCenter = 1u << 4,
  StyleInset = 1u << 5,
  StyleRanked = 1u << 6,
  StyleHairline = 1u << 7,
};
using StyleFlags = uint16_t;

inline bool hasStyle(StyleFlags flags, StyleFlag flag) {
  return (flags & static_cast<StyleFlags>(flag)) != 0;
}

struct Node {
  NodeType type = NodeType::Text;
  std::string id;
  std::string text;       // literal text; @foo means scalar binding
  std::string source;     // list/tab/tile/flow datasource
  std::string titleField; // row title field
  std::string subtitleField;
  std::string valueField;
  std::string action;     // activate/change/tap action
  std::string secondaryAction;
  StyleFlags style = StyleNone;
  int height = 0;
  int pageSize = 0;
  int value = 0;
  int max = 0;
  bool selectable = true;
  bool bold = false;

  // Button-bar labels/actions. Slots map to Back/Confirm/Left/Right.
  std::string labels[4];
  std::string actions[4];
};

struct HitRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  bool contains(int px, int py) const {
    return width > 0 && height > 0 && px >= x && px < x + width && py >= y && py < y + height;
  }
};

// Shared provider footer geometry. Visual rectangles are also touch targets;
// active slots are packed so a missing Confirm/Left action does not leave a
// dead quarter of the footer between Back and Refresh.
struct ProviderFooterLayout {
  static constexpr int kHeight = 64;
  static constexpr int kOuterMargin = 16;
  static constexpr int kGap = 12;

  int top = 0;
  int height = 0;
  int count = 0;
  HitRect buttons[4]{};
  int slots[4] = {-1, -1, -1, -1};

  static ProviderFooterLayout make(int screenWidth, int screenHeight, const bool active[4]) {
    ProviderFooterLayout out;
    out.height = std::min(kHeight, std::max(0, screenHeight));
    out.top = std::max(0, screenHeight - out.height);
    if (!active || screenWidth <= 2 * kOuterMargin) return out;
    for (int i = 0; i < 4; ++i) {
      if (active[i]) out.slots[out.count++] = i;
    }
    if (out.count == 0) return out;

    const int available = std::max(1, screenWidth - 2 * kOuterMargin - kGap * (out.count - 1));
    const int baseWidth = std::max(1, available / out.count);
    const int remainder = std::max(0, available - baseWidth * out.count);
    int x = kOuterMargin;
    for (int i = 0; i < out.count; ++i) {
      const int width = baseWidth + (i == out.count - 1 ? remainder : 0);
      out.buttons[i] = {x, out.top, width, out.height};
      x += width + kGap;
    }
    return out;
  }

  int buttonAt(int x, int y) const {
    for (int i = 0; i < count; ++i) {
      if (buttons[i].contains(x, y)) return slots[i];
    }
    return -1;
  }
};

// Three columns give a four-CJK label a real 128px nominal text lane at the
// 480px panel width. Drawing and hit-testing both use this layout.
struct ProviderTileLayout {
  static constexpr int kColumns = 3;
  static constexpr int kGap = 6;
  static constexpr int kLabelPadding = 4;
  static constexpr int kFourCjkNominalWidth = 128;

  int top = 0;
  int height = 0;
  int pad = 0;
  int count = 0;
  int rows = 0;
  int cellWidth = 0;
  int cellHeight = 0;

  static ProviderTileLayout make(int screenWidth, int top0, int height0, int count0, int pad0) {
    ProviderTileLayout out;
    out.top = top0;
    out.height = std::max(0, height0);
    out.pad = std::max(0, pad0);
    out.count = std::max(0, std::min(8, count0));
    out.rows = std::max(1, (out.count + kColumns - 1) / kColumns);
    const int innerWidth = std::max(1, screenWidth - 2 * out.pad);
    out.cellWidth = std::max(1, (innerWidth - kGap * (kColumns - 1)) / kColumns);
    out.cellHeight = std::max(1, (out.height - kGap * (out.rows - 1)) / out.rows);
    return out;
  }

  HitRect rectFor(int index0) const {
    if (index0 < 0 || index0 >= count) return {};
    return {pad + (index0 % kColumns) * (cellWidth + kGap),
            top + (index0 / kColumns) * (cellHeight + kGap), cellWidth, cellHeight};
  }

  int indexAt(int x, int y) const {
    const int gridWidth = cellWidth * kColumns + kGap * (kColumns - 1);
    if (count <= 0 || x < pad || y < top || x >= pad + gridWidth || y >= top + height) return -1;
    const int localX = x - pad;
    const int localY = y - top;
    const int stepX = cellWidth + kGap;
    const int stepY = cellHeight + kGap;
    const int col = localX / stepX;
    const int row = localY / stepY;
    if (col < 0 || col >= kColumns || row < 0 || row >= rows || localX % stepX >= cellWidth ||
        localY % stepY >= cellHeight) {
      return -1;
    }
    const int index = row * kColumns + col;
    return index < count ? index : -1;
  }

  int labelMaxWidth() const { return std::max(1, cellWidth - 2 * kLabelPadding); }
};

struct Screen {
  std::string id;
  std::string title;      // literal; @foo means scalar binding
  StyleFlags style = StyleNone;
  std::vector<Node> nodes;
};

struct Document {
  int version = kSchemaVersion;
  std::string startScreen;
  std::string theme;      // optional semantic theme name, e.g. "wap"

  // Root-level viewport/typography policy. These are deliberately bounded
  // declarative hints, not arbitrary CSS. fullscreen transfers the bottom
  // viewport from global Murphy Back/Home chrome to the app's own <buttons>,
  // while uiScale changes only M4UiText draw-time metrics (60..125%).
  bool fullscreen = false;
  int uiScalePercent = 100;

  std::vector<Screen> screens;
};

enum class ParseError : uint8_t {
  None = 0,
  Empty,
  TooLarge,
  Syntax,
  UnsupportedVersion,
  BadRoot,
  BadScreen,
  BadNode,
  TooManyScreens,
  TooManyNodes,
  AttributeTooLong,
  StringTooLong,
  DuplicateScreen,
  MissingStartScreen,
};

const char* errorKey(ParseError e);

struct ParseResult {
  Document document;
  ParseError error = ParseError::None;
  size_t offset = 0;
  std::string detail;
  explicit operator bool() const { return error == ParseError::None; }
};

// Parse the bounded M4 Native UI XML subset. This is deliberately not a full
// XML implementation: no DTD/entities/namespaces/CDATA, no executable
// expressions, and UI component nodes must be self-closing. The restricted
// grammar makes package parsing deterministic and small enough for ESP32-S3.
ParseResult parse(const char* xml, size_t len, const Limits& limits = {});

const Screen* findScreen(const Document& doc, const std::string& id);

}  // namespace M4NativeUi

#pragma once

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

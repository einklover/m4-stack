#include "apps/native/M4NativeUi.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace M4NativeUi {
namespace {

struct Attr {
  std::string key;
  std::string value;
};

struct Tag {
  std::string name;
  std::vector<Attr> attrs;
  bool closing = false;
  bool selfClosing = false;
};

bool isNameStart(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return std::isalpha(u) || c == '_' || c == ':';
}

bool isNameChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == ':';
}

void skipWs(const char* s, size_t n, size_t& p) {
  while (p < n && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
}

bool appendUtf8(uint32_t cp, std::string& out) {
  if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
  return true;
}

bool decodeEntity(const char* s, size_t n, size_t& p, std::string& out) {
  if (p >= n || s[p] != '&') return false;
  const size_t begin = p++;
  const size_t semi = [&]() {
    size_t q = p;
    while (q < n && q - begin <= 10 && s[q] != ';') ++q;
    return q;
  }();
  if (semi >= n || s[semi] != ';') return false;
  const std::string e(s + p, semi - p);
  if (e == "amp") out.push_back('&');
  else if (e == "lt") out.push_back('<');
  else if (e == "gt") out.push_back('>');
  else if (e == "quot") out.push_back('"');
  else if (e == "apos") out.push_back('\'');
  else if (!e.empty() && e[0] == '#') {
    int base = 10;
    size_t first = 1;
    if (e.size() > 2 && (e[1] == 'x' || e[1] == 'X')) {
      base = 16;
      first = 2;
    }
    if (first >= e.size()) return false;
    char* end = nullptr;
    const unsigned long cp = std::strtoul(e.c_str() + first, &end, base);
    if (!end || *end != '\0' || !appendUtf8(static_cast<uint32_t>(cp), out)) return false;
  } else {
    return false;
  }
  p = semi + 1;
  return true;
}

bool parseName(const char* s, size_t n, size_t& p, std::string& out) {
  out.clear();
  if (p >= n || !isNameStart(s[p])) return false;
  const size_t b = p++;
  while (p < n && isNameChar(s[p])) ++p;
  out.assign(s + b, p - b);
  return true;
}

bool parseQuoted(const char* s, size_t n, size_t& p, std::string& out, const Limits& lim,
                 ParseError& err) {
  out.clear();
  if (p >= n || (s[p] != '"' && s[p] != '\'')) return false;
  const char quote = s[p++];
  while (p < n && s[p] != quote) {
    if (out.size() >= lim.maxStringBytes) {
      err = ParseError::StringTooLong;
      return false;
    }
    if (s[p] == '&') {
      if (!decodeEntity(s, n, p, out)) return false;
      if (out.size() > lim.maxStringBytes) {
        err = ParseError::StringTooLong;
        return false;
      }
    } else {
      out.push_back(s[p++]);
    }
  }
  if (p >= n || s[p] != quote) return false;
  ++p;
  return true;
}

bool parseTag(const char* s, size_t n, size_t& p, Tag& tag, const Limits& lim,
              ParseError& err) {
  tag = {};
  if (p >= n || s[p] != '<') return false;
  ++p;
  if (p < n && s[p] == '/') {
    tag.closing = true;
    ++p;
  }
  skipWs(s, n, p);
  if (!parseName(s, n, p, tag.name)) return false;
  size_t attrBytes = 0;
  while (p < n) {
    skipWs(s, n, p);
    if (p >= n) return false;
    if (s[p] == '>') {
      ++p;
      return true;
    }
    if (s[p] == '/' && p + 1 < n && s[p + 1] == '>') {
      p += 2;
      tag.selfClosing = true;
      return true;
    }
    if (tag.closing) return false;
    Attr a;
    const size_t ab = p;
    if (!parseName(s, n, p, a.key)) return false;
    skipWs(s, n, p);
    if (p >= n || s[p++] != '=') return false;
    skipWs(s, n, p);
    if (!parseQuoted(s, n, p, a.value, lim, err)) return false;
    attrBytes += p - ab;
    if (attrBytes > lim.maxAttrBytes) {
      err = ParseError::AttributeTooLong;
      return false;
    }
    tag.attrs.push_back(std::move(a));
  }
  return false;
}

const std::string* attr(const Tag& t, const char* key) {
  for (const auto& a : t.attrs) {
    if (a.key == key) return &a.value;
  }
  return nullptr;
}

std::string attrOr(const Tag& t, const char* key, const char* fallback = "") {
  const auto* v = attr(t, key);
  return v ? *v : std::string(fallback);
}

bool boolAttr(const Tag& t, const char* key, bool fallback) {
  const auto* v = attr(t, key);
  if (!v) return fallback;
  std::string s = *v;
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (s == "1" || s == "true" || s == "yes") return true;
  if (s == "0" || s == "false" || s == "no") return false;
  return fallback;
}

int intAttr(const Tag& t, const char* key, int fallback) {
  const auto* v = attr(t, key);
  if (!v || v->empty()) return fallback;
  char* end = nullptr;
  const long x = std::strtol(v->c_str(), &end, 10);
  if (!end || *end != '\0') return fallback;
  if (x < -32768 || x > 32767) return fallback;
  return static_cast<int>(x);
}

StyleFlags parseStyleFlags(const std::string& classes) {
  StyleFlags flags = StyleNone;
  size_t p = 0;
  while (p < classes.size()) {
    while (p < classes.size() && std::isspace(static_cast<unsigned char>(classes[p]))) ++p;
    const size_t begin = p;
    while (p < classes.size() && !std::isspace(static_cast<unsigned char>(classes[p]))) ++p;
    if (begin == p) continue;
    const std::string token = classes.substr(begin, p - begin);
    if (token == "compact") flags |= StyleCompact;
    else if (token == "hero") flags |= StyleHero;
    else if (token == "section") flags |= StyleSection;
    else if (token == "muted" || token == "meta") flags |= StyleMuted;
    else if (token == "center") flags |= StyleCenter;
    else if (token == "inset") flags |= StyleInset;
    else if (token == "ranked") flags |= StyleRanked;
    else if (token == "hairline") flags |= StyleHairline;
  }
  return flags;
}

bool isComponentName(const std::string& n) {
  return n == "text" || n == "flowText" || n == "image" || n == "list" || n == "tiles" || n == "tabs" ||
         n == "progress" || n == "spacer" || n == "divider" || n == "buttons";
}

NodeType nodeTypeFor(const std::string& n) {
  if (n == "flowText") return NodeType::FlowText;
  if (n == "image") return NodeType::Image;
  if (n == "list") return NodeType::List;
  if (n == "tiles") return NodeType::Tiles;
  if (n == "tabs") return NodeType::Tabs;
  if (n == "progress") return NodeType::Progress;
  if (n == "spacer") return NodeType::Spacer;
  if (n == "divider") return NodeType::Divider;
  if (n == "buttons") return NodeType::Buttons;
  return NodeType::Text;
}

Node makeNode(const Tag& t) {
  Node n;
  n.type = nodeTypeFor(t.name);
  n.id = attrOr(t, "id");
  n.text = attrOr(t, "text");
  n.source = attrOr(t, "source");
  n.titleField = attrOr(t, "titleField", "title");
  n.subtitleField = attrOr(t, "subtitleField");
  n.valueField = attrOr(t, "valueField");
  n.action = attrOr(t, "action");
  if (n.action.empty()) n.action = attrOr(t, "onActivate");
  if (n.action.empty()) n.action = attrOr(t, "onChange");
  n.secondaryAction = attrOr(t, "secondaryAction");
  n.style = parseStyleFlags(attrOr(t, "class"));
  n.height = intAttr(t, "height", 0);
  n.pageSize = intAttr(t, "pageSize", 0);
  n.value = intAttr(t, "value", 0);
  n.max = intAttr(t, "max", 0);
  n.selectable = boolAttr(t, "selectable", true);
  n.bold = boolAttr(t, "bold", false);

  static constexpr const char* kLabelKeys[4] = {"back", "primary", "left", "right"};
  static constexpr const char* kActionKeys[4] = {"onBack", "onPrimary", "onLeft", "onRight"};
  if (n.type == NodeType::Buttons) {
    for (int i = 0; i < 4; ++i) {
      n.labels[i] = attrOr(t, kLabelKeys[i]);
      n.actions[i] = attrOr(t, kActionKeys[i]);
    }
  }
  return n;
}

bool onlyWhitespace(const char* s, size_t a, size_t b) {
  for (size_t i = a; i < b; ++i) {
    if (!std::isspace(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

}  // namespace

const char* errorKey(ParseError e) {
  switch (e) {
    case ParseError::None: return "none";
    case ParseError::Empty: return "empty";
    case ParseError::TooLarge: return "too_large";
    case ParseError::Syntax: return "syntax";
    case ParseError::UnsupportedVersion: return "unsupported_version";
    case ParseError::BadRoot: return "bad_root";
    case ParseError::BadScreen: return "bad_screen";
    case ParseError::BadNode: return "bad_node";
    case ParseError::TooManyScreens: return "too_many_screens";
    case ParseError::TooManyNodes: return "too_many_nodes";
    case ParseError::AttributeTooLong: return "attribute_too_long";
    case ParseError::StringTooLong: return "string_too_long";
    case ParseError::DuplicateScreen: return "duplicate_screen";
    case ParseError::MissingStartScreen: return "missing_start_screen";
  }
  return "unknown";
}

ParseResult parse(const char* xml, size_t len, const Limits& lim) {
  ParseResult r;
  if (!xml || len == 0) {
    r.error = ParseError::Empty;
    return r;
  }
  if (len > lim.maxBytes) {
    r.error = ParseError::TooLarge;
    return r;
  }

  size_t p = 0;
  bool rootOpen = false;
  bool rootClosed = false;
  Screen* current = nullptr;
  while (p < len) {
    if (xml[p] != '<') {
      const size_t b = p;
      while (p < len && xml[p] != '<') ++p;
      if (!onlyWhitespace(xml, b, p)) {
        r.error = ParseError::Syntax;
        r.offset = b;
        r.detail = "text_nodes_not_supported";
        return r;
      }
      continue;
    }

    if (p + 1 < len && xml[p + 1] == '?') {
      const size_t end = std::string(xml + p, len - p).find("?>");
      if (end == std::string::npos) {
        r.error = ParseError::Syntax;
        r.offset = p;
        return r;
      }
      p += end + 2;
      continue;
    }
    if (p + 3 < len && xml[p + 1] == '!' && xml[p + 2] == '-' && xml[p + 3] == '-') {
      const size_t end = std::string(xml + p, len - p).find("-->");
      if (end == std::string::npos) {
        r.error = ParseError::Syntax;
        r.offset = p;
        return r;
      }
      p += end + 3;
      continue;
    }

    const size_t tagOffset = p;
    Tag t;
    ParseError tagErr = ParseError::None;
    if (!parseTag(xml, len, p, t, lim, tagErr)) {
      r.error = tagErr == ParseError::None ? ParseError::Syntax : tagErr;
      r.offset = tagOffset;
      return r;
    }

    if (!rootOpen) {
      if (t.closing || t.selfClosing || t.name != "m4ui") {
        r.error = ParseError::BadRoot;
        r.offset = tagOffset;
        return r;
      }
      const int version = intAttr(t, "version", kSchemaVersion);
      if (version != kSchemaVersion) {
        r.error = ParseError::UnsupportedVersion;
        r.offset = tagOffset;
        return r;
      }
      r.document.version = version;
      r.document.startScreen = attrOr(t, "start");
      r.document.theme = attrOr(t, "theme");
      r.document.fullscreen = boolAttr(t, "fullscreen", false);
      r.document.uiScalePercent = std::max(60, std::min(125, intAttr(t, "uiScale", 100)));
      rootOpen = true;
      continue;
    }

    if (t.name == "m4ui") {
      if (!t.closing || current) {
        r.error = ParseError::BadRoot;
        r.offset = tagOffset;
        return r;
      }
      rootClosed = true;
      break;
    }

    if (t.name == "screen") {
      if (t.closing) {
        if (!current) {
          r.error = ParseError::BadScreen;
          r.offset = tagOffset;
          return r;
        }
        current = nullptr;
        continue;
      }
      if (t.selfClosing || current) {
        r.error = ParseError::BadScreen;
        r.offset = tagOffset;
        return r;
      }
      if (r.document.screens.size() >= lim.maxScreens) {
        r.error = ParseError::TooManyScreens;
        r.offset = tagOffset;
        return r;
      }
      Screen s;
      s.id = attrOr(t, "id");
      s.title = attrOr(t, "title");
      s.style = parseStyleFlags(attrOr(t, "class"));
      if (s.id.empty()) {
        r.error = ParseError::BadScreen;
        r.offset = tagOffset;
        r.detail = "missing_screen_id";
        return r;
      }
      for (const auto& old : r.document.screens) {
        if (old.id == s.id) {
          r.error = ParseError::DuplicateScreen;
          r.offset = tagOffset;
          return r;
        }
      }
      r.document.screens.push_back(std::move(s));
      current = &r.document.screens.back();
      continue;
    }

    if (!current || t.closing || !t.selfClosing || !isComponentName(t.name)) {
      r.error = ParseError::BadNode;
      r.offset = tagOffset;
      r.detail = t.name;
      return r;
    }
    if (current->nodes.size() >= lim.maxNodesPerScreen) {
      r.error = ParseError::TooManyNodes;
      r.offset = tagOffset;
      return r;
    }
    Node n = makeNode(t);
    if ((n.type == NodeType::List || n.type == NodeType::Tiles || n.type == NodeType::Tabs) &&
        n.source.empty()) {
      r.error = ParseError::BadNode;
      r.offset = tagOffset;
      r.detail = "source_required";
      return r;
    }
    current->nodes.push_back(std::move(n));
  }

  if (!rootOpen || !rootClosed || current != nullptr || r.document.screens.empty()) {
    r.error = !rootOpen ? ParseError::BadRoot : ParseError::Syntax;
    r.offset = p;
    return r;
  }
  if (r.document.startScreen.empty()) r.document.startScreen = r.document.screens.front().id;
  if (!findScreen(r.document, r.document.startScreen)) {
    r.error = ParseError::MissingStartScreen;
    r.offset = p;
    return r;
  }
  return r;
}

const Screen* findScreen(const Document& doc, const std::string& id) {
  for (const auto& s : doc.screens) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

}  // namespace M4NativeUi

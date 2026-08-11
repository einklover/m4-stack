#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

namespace M4NativeProviderExplore {

// Provider-neutral equivalent of Legado's exploreUrl: the UI only sees a
// bounded list of categories; the opaque key is interpreted by the provider
// discovery adapter. No provider URL/protocol detail leaks into M4UI.
//
// Category tiles intentionally use one concise line. Four-CJK-character labels
// fit the 4-column M4 tile at the stable UI_10 chrome size, while detailed
// genre/status copy stays out of the touch target so it cannot crowd the label.
struct Category {
  const char* key;
  const char* title;
  const char* subtitle;
};

inline constexpr Category kJjwxc[] = {
    {"14000019", "古代言情", ""},
    {"15000018", "现代言情", ""},
    {"80000026", "幻想言情", ""},
    {"21000025", "纯爱小说", ""},
    {"22000024", "百合小说", ""},
    {"80000272", "无CP", ""},
    {"19000034", "科幻悬疑", ""},
    {"14000038", "完结精选", ""},
};

// Fanqie keys are <gender>:<category_id>. The IDs are deliberately kept in
// this adapter table so XML/controller code remains provider-neutral.
inline constexpr Category kFanqie[] = {
    {"1:516", "都市异能", ""},
    {"1:511", "东方玄幻", ""},
    {"1:259", "奇幻仙侠", ""},
    {"1:273", "历史古代", ""},
    {"1:751", "悬疑灵异", ""},
    {"1:506", "探案推理", ""},
    {"1:718", "动漫衍生", ""},
    {"1:11", "乡村生活", ""},
};

// Legado has no useful explore taxonomy on M4: the host only syncs the LAN
// bookshelf (/getBookshelf). Keep category count at zero so tiles never appear.
inline size_t count(const std::string& providerId) {
  if (providerId == "jjwxc") return sizeof(kJjwxc) / sizeof(kJjwxc[0]);
  if (providerId == "fanqie") return sizeof(kFanqie) / sizeof(kFanqie[0]);
  return 0;
}

inline bool at(const std::string& providerId, size_t index0, Category& out) {
  if (providerId == "jjwxc") {
    if (index0 >= count(providerId)) return false;
    out = kJjwxc[index0];
    return true;
  }
  if (providerId == "fanqie") {
    if (index0 >= count(providerId)) return false;
    out = kFanqie[index0];
    return true;
  }
  return false;
}

inline bool find(const std::string& providerId, const std::string& key, Category& out) {
  for (size_t i = 0; i < count(providerId); ++i) {
    Category c{};
    if (at(providerId, i, c) && key == c.key) {
      out = c;
      return true;
    }
  }
  return false;
}

inline std::string defaultKey(const std::string& providerId) {
  Category c{};
  return at(providerId, 0, c) ? std::string(c.key) : std::string();
}

inline bool decodeFanqieKey(const std::string& key, int& gender, int& categoryId) {
  gender = 0;
  categoryId = 0;
  const size_t colon = key.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= key.size()) return false;
  const std::string genderText = key.substr(0, colon);
  const std::string categoryText = key.substr(colon + 1);
  char* end = nullptr;
  const long g = std::strtol(genderText.c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  end = nullptr;
  const long c = std::strtol(categoryText.c_str(), &end, 10);
  if (!end || *end != '\0' || g < 1 || g > 2 || c <= 0 || c > 100000) return false;
  gender = static_cast<int>(g);
  categoryId = static_cast<int>(c);
  return true;
}

}  // namespace M4NativeProviderExplore

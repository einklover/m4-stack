#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace M4NovelProvider {

// A deliberately small, provider-neutral contract for the common novel-site
// shape used by JJWXC, Fanqie and future converted sources. It mirrors the
// useful parts of Legado's source model without embedding a browser/JS engine.
//
//   exploreUrl/ruleExplore -> ExploreCategory + BookCard
//   bookUrl/ruleBookInfo   -> BookDetail
//   tocUrl/ruleToc         -> existing ChapterCatalogSpec/FileRows
//   chapterUrl/ruleContent -> existing native chapter pipeline
//
// UI, cache, reader, progress and network lifecycle are host-owned.

enum Capability : uint32_t {
  CapabilityNone = 0,
  CapabilityExplore = 1u << 0,
  CapabilitySearch = 1u << 1,
  CapabilityBookDetail = 1u << 2,
  CapabilityCatalog = 1u << 3,
  CapabilityChapter = 1u << 4,
  CapabilityLogin = 1u << 5,
};

struct ExploreCategory {
  std::string key;       // opaque provider query key; never a URL in the UI
  std::string title;     // e.g. 古言 / 都市
  std::string subtitle;  // e.g. 天作之合 · 连载
};

// Standard discovery/search row. Keep this compact: large result sets are
// persisted as FileRows and only one row is materialized by the controller.
struct BookCard {
  std::string bookId;
  std::string title;
  std::string author;
  std::string coverUrl;  // optional provider URL; host owns fetch/decode/cache
  std::string meta;   // category/status/update summary
  std::string value;  // rank/progress/word-count display value
  uint32_t flags = 0;
};

// Unified detail-page model. Providers may fill only fields they can obtain
// cheaply; the system detail template remains usable with title+author alone.
struct BookDetail {
  std::string providerId;
  std::string bookId;
  std::string title;
  std::string author;
  std::string coverUrl;  // optional provider URL; host owns fetch/decode/cache
  std::string intro;
  std::string kind;
  std::string status;
  std::string wordCount;
  std::string lastChapter;
  uint32_t flags = 0;
};

struct Descriptor {
  std::string providerId;
  std::string displayName;
  uint32_t capabilities = CapabilityExplore | CapabilityBookDetail |
                          CapabilityCatalog | CapabilityChapter;
};

inline bool has(uint32_t caps, Capability cap) {
  return (caps & static_cast<uint32_t>(cap)) != 0;
}

// Stable datasource names consumed by the shared Native UI templates.
inline constexpr const char* kSourceCategories = "provider.categories";
inline constexpr const char* kSourceRecommend = "provider.recommend";
inline constexpr const char* kSourceSearch = "provider.searchResults";

// Stable symbolic actions. A future source converter only needs to implement
// the provider-side stages; it never needs to author a custom Activity.
inline constexpr const char* kActionSelectCategory = "provider.selectCategory";
inline constexpr const char* kActionOpenBook = "provider.openBook";
inline constexpr const char* kActionRefresh = "provider.refresh";

}  // namespace M4NovelProvider

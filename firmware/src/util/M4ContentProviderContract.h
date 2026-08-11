#pragma once

// Provider-agnostic contract for plugin content sources (WeRead first).
// Host-testable pure helpers — no Arduino / SD / network.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace M4ContentProvider {

inline constexpr size_t kMaxProviderIdLen = 32;
inline constexpr size_t kMaxBookIdLen = 64;
inline constexpr size_t kMaxChapterUidLen = 64;
inline constexpr size_t kMaxTitleLen = 128;
inline constexpr size_t kMaxPathLen = 192;
inline constexpr size_t kMaxErrorLen = 48;
inline constexpr size_t kMaxInlineChapters = 512;
inline constexpr size_t kMaxCatalogChapters = 200000;
// Source compatibility: callers using the old name mean an inline Lua table.
inline constexpr size_t kMaxChapters = kMaxInlineChapters;

enum ProviderCapability : uint32_t {
  CapabilityNone = 0,
  CapabilityChapterCatalog = 1u << 0,
  CapabilityRandomChapter = 1u << 1,
  CapabilityPrefetch = 1u << 2,
  CapabilityOfflineCache = 1u << 3,
};

struct CachePolicy {
  size_t maxChapterBytes = 2u * 1024u * 1024u;
  int prefetchAhead = 1;
  int retainBehind = 1;
  int maxReadyChapters = 4;
  bool offlineReopen = true;
};

// One canonical reader position shared by the plugin, native reader, history,
// and cache layers. chapterIndex0/chapterUid identify the content; byteOffset
// is the durable position. page fields are optional presentation metadata and
// must never replace byteOffset for resume.
struct ReadPosition {
  int chapterIndex0 = 0;
  std::string chapterUid;
  size_t byteOffset = 0;
  bool hasByteOffset = false;
  int page0 = 0;
  int pageCount = 0;
};

enum class ChapterReady : uint8_t {
  Missing = 0,
  Fetching = 1,
  Ready = 2,
  Error = 3,
};

struct ChapterMeta {
  std::string uid;
  std::string title;
};

enum class ChapterCatalogKind : uint8_t { Inline = 0, FileRows = 1 };

// Large catalogs live as tab-separated rows in app data and are paged through
// M4FileRowSource. They are not copied into BookSpec or the runtime registry.
// File fields are zero-based; uid is required, title is optional.
struct ChapterCatalogSpec {
  ChapterCatalogKind kind = ChapterCatalogKind::Inline;
  size_t chapterCount = 0;       // 0 means chapters.size() for Inline
  std::string fileRelPath;       // required for FileRows
  int uidField0 = 0;
  int titleField0 = 1;
  // Optional 0-based TSV column for VIP/lock flag (e.g. jjwxc isvip at index 3).
  // -1 = disabled. When set and value is non-zero / non-"0", native TOC prefixes "VIP ".
  int vipField0 = -1;
};

struct BookSpec {
  std::string providerId;  // e.g. "weread" — not a filesystem path
  std::string bookId;
  std::string title;
  std::string appId;
  std::vector<ChapterMeta> chapters;
  ChapterCatalogSpec catalog{};
  int currentIndex0 = 0;
  uint32_t capabilities = CapabilityChapterCatalog | CapabilityRandomChapter | CapabilityPrefetch |
                          CapabilityOfflineCache;
  CachePolicy cachePolicy{};
};

struct ChapterStatus {
  std::string providerId;  // required for updates (unambiguous multi-provider bind)
  std::string bookId;
  std::string chapterUid;
  int index0 = -1;
  ChapterReady state = ChapterReady::Missing;
  std::string cacheRelPath;  // app-data relative .txt when Ready
  int pct = 0;               // 0..100 while Fetching
  std::string error;         // stable code; never secrets
};

struct PrefetchWork {
  bool valid = false;
  std::string providerId;
  std::string bookId;
  std::string chapterUid;
  int index0 = -1;
  // For a file-backed catalog chapterUid is intentionally empty until the
  // provider pump resolves this row. The work item carries the bounded row
  // source locator and field mapping needed for that explicit lookup.
  ChapterCatalogSpec catalog{};
};

struct NextChapterDecision {
  enum class Action : uint8_t { None, OpenReady, WaitOverlay, RequestAndWait } action = Action::None;
  ChapterStatus next;
  bool shouldRequestPrefetch = false;
};

struct HistoryEntry {
  std::string providerId;
  std::string bookId;
  std::string title;
  std::string chapterUid;
  int chapterIndex0 = 0;
  size_t byteOffset = 0;
  bool hasByteOffset = false;
  int page0 = 0;
  int pageCount = 0;
  std::string cacheRelPath;
};

enum class ValidationError : uint8_t {
  None = 0,
  BadProviderId,
  BadBookId,
  BadAppId,
  TooManyChapters,
  BadChapterUid,
  DuplicateChapterUid,
  BadTitle,
  BadCurrentIndex,
  BadCachePolicy,
  BadCatalog,
};

// --- ID / URI helpers (no I/O) ---

inline bool idOk(const char* s, size_t maxLen) {
  if (!s || !s[0]) return false;
  const size_t n = std::strlen(s);
  if (n > maxLen) return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == ' ' || c == '/' || c == '\\' || c == '?' || c == '#') return false;
  }
  return true;
}

inline bool isSafeCacheRelPath(const char* rel) {
  if (!rel || !rel[0]) return false;
  if (rel[0] == '/' || rel[0] == '\\') return false;
  if (std::strstr(rel, "..") != nullptr) return false;
  const size_t n = std::strlen(rel);
  if (n > kMaxPathLen || n < 5) return false;
  // Must end in .txt (case-insensitive).
  const char* e = rel + n - 4;
  return (e[0] == '.') && (e[1] == 't' || e[1] == 'T') && (e[2] == 'x' || e[2] == 'X') &&
         (e[3] == 't' || e[3] == 'T');
}

inline bool isSafeDataRelPath(const char* rel) {
  if (!rel || !rel[0] || rel[0] == '/' || rel[0] == '\\') return false;
  const size_t n = std::strlen(rel);
  if (n > kMaxPathLen || std::strstr(rel, "..") != nullptr) return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(rel[i]);
    if (c < 0x20 || c == '\\' || c == ':') return false;
  }
  return true;
}

// historyUri: m4cp://<providerId>/<bookId>
inline std::string makeHistoryUri(const char* providerId, const char* bookId) {
  if (!idOk(providerId, kMaxProviderIdLen) || !idOk(bookId, kMaxBookIdLen)) return {};
  return std::string("m4cp://") + providerId + "/" + bookId;
}

inline bool parseHistoryUri(const char* uri, std::string& providerIdOut, std::string& bookIdOut) {
  providerIdOut.clear();
  bookIdOut.clear();
  if (!uri) return false;
  static constexpr char kPrefix[] = "m4cp://";
  const size_t plen = sizeof(kPrefix) - 1;
  if (std::strncmp(uri, kPrefix, plen) != 0) return false;
  const char* rest = uri + plen;
  const char* slash = std::strchr(rest, '/');
  if (!slash || slash == rest || !slash[1]) return false;
  providerIdOut.assign(rest, static_cast<size_t>(slash - rest));
  bookIdOut = slash + 1;
  if (bookIdOut.find('/') != std::string::npos) return false;
  return idOk(providerIdOut.c_str(), kMaxProviderIdLen) && idOk(bookIdOut.c_str(), kMaxBookIdLen);
}

inline bool isHistoryUri(const char* path) {
  std::string a, b;
  return parseHistoryUri(path, a, b);
}

inline ValidationError validateBookSpec(const BookSpec& spec) {
  if (!idOk(spec.providerId.c_str(), kMaxProviderIdLen)) return ValidationError::BadProviderId;
  if (!idOk(spec.bookId.c_str(), kMaxBookIdLen)) return ValidationError::BadBookId;
  if (!spec.appId.empty() && !idOk(spec.appId.c_str(), 96)) return ValidationError::BadAppId;
  if (spec.title.size() > kMaxTitleLen) return ValidationError::BadTitle;
  const bool fileCatalog = spec.catalog.kind == ChapterCatalogKind::FileRows;
  if (!fileCatalog && spec.chapters.size() > kMaxInlineChapters) return ValidationError::TooManyChapters;
  if (fileCatalog && (!spec.chapters.empty() || spec.catalog.chapterCount == 0 ||
                      spec.catalog.chapterCount > kMaxCatalogChapters ||
                      !isSafeDataRelPath(spec.catalog.fileRelPath.c_str()) ||
                      spec.catalog.uidField0 < 0 || spec.catalog.uidField0 > 15 ||
                      spec.catalog.titleField0 < -1 || spec.catalog.titleField0 > 15)) {
    return ValidationError::BadCatalog;
  }
  const size_t chapterCount = fileCatalog ? spec.catalog.chapterCount : spec.chapters.size();
  if (chapterCount != 0 &&
      (spec.currentIndex0 < 0 || static_cast<size_t>(spec.currentIndex0) >= chapterCount)) {
    return ValidationError::BadCurrentIndex;
  }
  if (spec.cachePolicy.maxChapterBytes == 0 || spec.cachePolicy.prefetchAhead < 0 ||
      spec.cachePolicy.prefetchAhead > 4 || spec.cachePolicy.retainBehind < 0 ||
      spec.cachePolicy.retainBehind > 8 || spec.cachePolicy.maxReadyChapters < 1 ||
      spec.cachePolicy.maxReadyChapters > 32) {
    return ValidationError::BadCachePolicy;
  }
  for (size_t i = 0; i < spec.chapters.size(); ++i) {
    const ChapterMeta& ch = spec.chapters[i];
    if (!idOk(ch.uid.c_str(), kMaxChapterUidLen)) return ValidationError::BadChapterUid;
    if (ch.title.size() > kMaxTitleLen) return ValidationError::BadTitle;
    for (size_t j = 0; j < i; ++j) {
      if (spec.chapters[j].uid == ch.uid) return ValidationError::DuplicateChapterUid;
    }
  }
  return ValidationError::None;
}

inline size_t bookChapterCount(const BookSpec& spec) {
  return spec.catalog.kind == ChapterCatalogKind::FileRows ? spec.catalog.chapterCount
                                                           : spec.chapters.size();
}

inline bool requiresCatalogResolve(const PrefetchWork& work) {
  return work.valid && work.chapterUid.empty() &&
         work.catalog.kind == ChapterCatalogKind::FileRows && work.index0 >= 0 &&
         static_cast<size_t>(work.index0) < work.catalog.chapterCount &&
         isSafeDataRelPath(work.catalog.fileRelPath.c_str());
}

inline bool validReadPosition(const ReadPosition& p, size_t chapterCount) {
  if (p.chapterIndex0 < 0 || static_cast<size_t>(p.chapterIndex0) >= chapterCount) return false;
  if (!p.chapterUid.empty() && !idOk(p.chapterUid.c_str(), kMaxChapterUidLen)) return false;
  if (p.page0 < 0 || p.pageCount < 0 || (p.pageCount > 0 && p.page0 >= p.pageCount)) return false;
  return true;
}

// progress.dat historically used native chapternum. Provider-backed TXT files
// each look like a one-chapter local book, so native chapternum remains zero;
// the provider chapter index is the only correct durable chapter identity.
inline int resolveProgressChapterIndex(bool pluginSessionActive, int pluginChapterIndex0,
                                       int nativeChapterIndex) {
  const int selected = pluginSessionActive && pluginChapterIndex0 >= 0 ? pluginChapterIndex0
                                                                      : nativeChapterIndex;
  return selected < 0 ? 0 : selected;
}

inline ChapterReady parseState(const char* s) {
  if (!s) return ChapterReady::Missing;
  if (std::strcmp(s, "ready") == 0) return ChapterReady::Ready;
  if (std::strcmp(s, "fetching") == 0) return ChapterReady::Fetching;
  if (std::strcmp(s, "error") == 0) return ChapterReady::Error;
  return ChapterReady::Missing;
}

inline const char* stateKey(ChapterReady s) {
  switch (s) {
    case ChapterReady::Ready:
      return "ready";
    case ChapterReady::Fetching:
      return "fetching";
    case ChapterReady::Error:
      return "error";
    default:
      return "missing";
  }
}

// Clamp pct; Ready forces 100; Missing forces 0.
inline int normalizePct(ChapterReady st, int pct) {
  if (st == ChapterReady::Ready) return 100;
  if (st == ChapterReady::Missing) return 0;
  if (pct < 0) return 0;
  if (pct > 100) return 100;
  return pct;
}

// Decide what the native reader should do at end-of-chapter (or TOC jump prep).
inline NextChapterDecision decideNextChapter(const ChapterStatus& next, bool userAdvanced) {
  NextChapterDecision d;
  d.next = next;
  if (!userAdvanced) {
    d.action = NextChapterDecision::Action::None;
    return d;
  }
  switch (next.state) {
    case ChapterReady::Ready:
      if (!next.cacheRelPath.empty() && isSafeCacheRelPath(next.cacheRelPath.c_str())) {
        d.action = NextChapterDecision::Action::OpenReady;
      } else {
        d.action = NextChapterDecision::Action::RequestAndWait;
        d.shouldRequestPrefetch = true;
      }
      break;
    case ChapterReady::Fetching:
      d.action = NextChapterDecision::Action::WaitOverlay;
      break;
    case ChapterReady::Error:
      d.action = NextChapterDecision::Action::WaitOverlay;
      d.shouldRequestPrefetch = true;  // allow retry
      break;
    case ChapterReady::Missing:
    default:
      d.action = NextChapterDecision::Action::RequestAndWait;
      d.shouldRequestPrefetch = true;
      break;
  }
  return d;
}

// Overlay copy must stay short / ASCII-safe for builtin subset when font_ok is false.
inline std::string overlayMessage(const ChapterStatus& st, bool fontOk) {
  if (st.state == ChapterReady::Ready) return {};
  if (st.state == ChapterReady::Error) {
    return fontOk ? "下一章加载失败" : "next chapter failed";
  }
  const int pct = normalizePct(st.state, st.pct);
  if (fontOk) {
    if (pct > 0 && pct < 100) {
      return std::string("加载下一章… ") + std::to_string(pct) + "%";
    }
    return "加载下一章…";
  }
  if (pct > 0 && pct < 100) {
    return std::string("loading next… ") + std::to_string(pct) + "%";
  }
  return "loading next…";
}

// Idle prefetch is one-shot: only enqueue a chapter that has never been tried.
// Error is deliberately foreground-only so a permanent network/auth failure
// cannot keep reopening TLS while the reader is active.
inline bool shouldIdlePrefetchNext(const ChapterStatus& next) {
  return next.state == ChapterReady::Missing;
}

}  // namespace M4ContentProvider

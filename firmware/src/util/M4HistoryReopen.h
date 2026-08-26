#pragma once

// History / Recent Books selection → open dispatch (host-testable).
// Recognizes m4cp://provider/book vs legacy filesystem paths.

#include "util/M4ContentProviderContract.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>

namespace M4HistoryReopen {

enum class Kind : uint8_t {
  LocalFile = 0,          // path is a real SD file (legacy)
  ProviderCached = 1,     // m4cp + cached chapter available → open native reader
  ProviderNeedsFetch = 2, // m4cp, no local cache → launch provider app / loading
  Invalid = 3,
};

struct Result {
  Kind kind = Kind::Invalid;
  std::string openPath;       // filesystem path for LocalFile / ProviderCached
  std::string appId;          // for NeedsFetch / PluginSession data root
  std::string providerId;
  std::string bookId;
  std::string chapterUid;
  std::string cacheRelPath;   // app-data relative when known
  std::string appDataRoot;    // /apps_data/<appId>
  std::string title;
  std::string overlayMessage; // non-blocking loading hint when NeedsFetch
};

using ProviderAppIdResolver = std::function<std::string(const std::string& providerId)>;

// Extract appId from /apps_data/<appId> or /apps_data/<appId>/...
inline bool appIdFromAppsDataAbs(const std::string& abs, std::string& appIdOut) {
  appIdOut.clear();
  static constexpr char kPrefix[] = "/apps_data/";
  if (abs.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
  const size_t start = sizeof(kPrefix) - 1;
  if (start >= abs.size()) return false;
  const size_t slash = abs.find('/', start);
  if (slash == std::string::npos) {
    appIdOut = abs.substr(start);
  } else {
    if (slash == start) return false;
    appIdOut = abs.substr(start, slash - start);
  }
  return !appIdOut.empty() && appIdOut.find("..") == std::string::npos;
}

inline std::string appDataRootFor(const std::string& appId) {
  if (appId.empty()) return {};
  return std::string("/apps_data/") + appId;
}

// /apps_data/<appId>/<rel> → rel
inline bool relPathUnderAppsData(const std::string& abs, const std::string& appId, std::string& relOut) {
  relOut.clear();
  const std::string root = appDataRootFor(appId) + "/";
  if (abs.compare(0, root.size(), root) != 0) return false;
  relOut = abs.substr(root.size());
  return M4ContentProvider::isSafeCacheRelPath(relOut.c_str());
}

// Best-effort chapterUid from .../ch_<uid>.txt
inline std::string chapterUidFromCacheRel(const std::string& rel) {
  const size_t slash = rel.find_last_of('/');
  std::string base = slash == std::string::npos ? rel : rel.substr(slash + 1);
  if (base.size() > 4 && (base.compare(base.size() - 4, 4, ".txt") == 0 ||
                          base.compare(base.size() - 4, 4, ".TXT") == 0)) {
    base = base.substr(0, base.size() - 4);
  }
  if (base.size() > 3 && base.compare(0, 3, "ch_") == 0) {
    return base.substr(3);
  }
  return base;
}

// exists(path): true if file exists on SD (injected for host tests).
// appInstalled(appId): true if m4x app is registered (injected).
// Prefer originalSourcePath as last chapter cache when present and exists.
inline Result resolveSelection(const std::string& path, const std::string& originalSourcePath,
                               const std::string& titleHint,
                               const std::function<bool(const std::string&)>& exists,
                               const std::function<bool(const std::string&)>& appInstalled) {
  Result r;
  r.title = titleHint;

  std::string providerId, bookId;
  if (!M4ContentProvider::parseHistoryUri(path.c_str(), providerId, bookId)) {
    // Legacy filesystem path.
    if (path.empty()) {
      r.kind = Kind::Invalid;
      return r;
    }
    if (exists && !exists(path)) {
      // Still allow open attempt — ReaderActivity will fail cleanly.
    }
    r.kind = Kind::LocalFile;
    r.openPath = path;
    return r;
  }

  r.providerId = providerId;
  r.bookId = bookId;
  r.kind = Kind::ProviderNeedsFetch;
  r.overlayMessage = "loading chapter…";

  // Prefer last-open cache path stored as originalSourcePath.
  if (!originalSourcePath.empty() && exists && exists(originalSourcePath)) {
    std::string appId;
    if (appIdFromAppsDataAbs(originalSourcePath, appId)) {
      r.appId = appId;
      r.appDataRoot = appDataRootFor(appId);
      std::string rel;
      if (relPathUnderAppsData(originalSourcePath, appId, rel)) {
        r.cacheRelPath = rel;
        r.chapterUid = chapterUidFromCacheRel(rel);
      }
      r.openPath = originalSourcePath;
      r.kind = Kind::ProviderCached;
      return r;
    }
    // Non-apps_data cache still openable as local file fallback.
    r.kind = Kind::ProviderCached;
    r.openPath = originalSourcePath;
    return r;
  }

  // Session reopen if still registered (same boot).
  M4ContentProvider::ChapterStatus st;
  // We need providerId — session is keyed by provider+book.
  // Include header only via forward - call from .cpp? Keep pure: optional inject.

  // App id: from author field passed as... caller can set originalSourcePath empty
  // and pass appId via title? Better: try standard apps_data layout probe.
  // Convention: try /apps_data/<candidate>/ — not enumerable without FS.
  // Store appId in originalSourcePath when missing cache as "app:<id>" prefix.
  if (originalSourcePath.compare(0, 4, "app:") == 0) {
    r.appId = originalSourcePath.substr(4);
    r.appDataRoot = appDataRootFor(r.appId);
  }

  if (!r.appId.empty() && appInstalled && appInstalled(r.appId)) {
    r.kind = Kind::ProviderNeedsFetch;
    r.overlayMessage = "opening provider…";
    return r;
  }

  // Unknown app / no cache: still ProviderNeedsFetch so UI can show loading;
  // dispatch may fail if app missing.
  r.kind = Kind::ProviderNeedsFetch;
  return r;
}

// Session-aware overload: if registry has Ready chapter, promote to Cached.
template <typename ResolveReopenFn>
inline Result resolveSelectionWithSession(const std::string& path, const std::string& originalSourcePath,
                                          const std::string& titleHint,
                                          const std::function<bool(const std::string&)>& exists,
                                          const std::function<bool(const std::string&)>& appInstalled,
                                          ResolveReopenFn resolveReopen) {
  Result r = resolveSelection(path, originalSourcePath, titleHint, exists, appInstalled);
  if (r.kind != Kind::ProviderNeedsFetch && r.kind != Kind::ProviderCached) return r;

  std::string providerId, bookId;
  if (!M4ContentProvider::parseHistoryUri(path.c_str(), providerId, bookId)) return r;

  M4ContentProvider::ChapterStatus st;
  if (resolveReopen(providerId, bookId, st) && st.state == M4ContentProvider::ChapterReady::Ready &&
      !st.cacheRelPath.empty()) {
    // Need appId for abs path — from existing r or apps_data probe via originalSourcePath.
    if (r.appId.empty() && !originalSourcePath.empty()) {
      (void)appIdFromAppsDataAbs(originalSourcePath, r.appId);
    }
    if (!r.appId.empty()) {
      r.appDataRoot = appDataRootFor(r.appId);
      r.cacheRelPath = st.cacheRelPath;
      r.chapterUid = st.chapterUid;
      r.openPath = r.appDataRoot + "/" + st.cacheRelPath;
      if (!exists || exists(r.openPath)) {
        r.kind = Kind::ProviderCached;
      }
    }
  }
  return r;
}

// True if s looks like a reverse-DNS m4x app id (not a short provider token).
inline bool looksLikeAppId(const std::string& s) {
  if (s.size() < 3) return false;
  if (s.find("..") != std::string::npos) return false;
  // Must contain a dot and only [A-Za-z0-9._-]
  bool hasDot = false;
  for (unsigned char c : s) {
    if (c == '.') {
      hasDot = true;
      continue;
    }
    if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
  }
  return hasDot;
}

// Resolve appId for history metadata. Never returns providerId tokens like "weread".
// Priority: explicitAppId → appDataRoot → cacheAbsPath under /apps_data/.
inline bool resolveHistoryAppId(const std::string& explicitAppId, const std::string& appDataRoot,
                                const std::string& cacheAbsPath, std::string& appIdOut) {
  appIdOut.clear();
  if (looksLikeAppId(explicitAppId)) {
    appIdOut = explicitAppId;
    return true;
  }
  std::string tmp;
  if (appIdFromAppsDataAbs(appDataRoot, tmp) && looksLikeAppId(tmp)) {
    appIdOut = tmp;
    return true;
  }
  if (appIdFromAppsDataAbs(cacheAbsPath, tmp) && looksLikeAppId(tmp)) {
    appIdOut = tmp;
    return true;
  }
  return false;
}

// Real history-selection path: inputs are exactly what Home/RecentBooks pass.
// URI/registry identity wins. authorAppIdHint is only the legacy fallback for
// old records that persisted the app id in RecentBook.author.
inline Result resolveFromRecentBookFields(const std::string& recentPath, const std::string& originalSourcePath,
                                          const std::string& title, const std::string& authorAppIdHint,
                                          const ProviderAppIdResolver& appIdForProvider,
                                          const std::function<bool(const std::string&)>& exists,
                                          const std::function<bool(const std::string&)>& appInstalled) {
  Result r = resolveSelection(recentPath, originalSourcePath, title, exists, appInstalled);
  if (r.kind != Kind::ProviderNeedsFetch && r.kind != Kind::ProviderCached) return r;

  if (r.appId.empty() && !r.providerId.empty() && appIdForProvider) {
    const std::string resolved = appIdForProvider(r.providerId);
    if (looksLikeAppId(resolved)) {
      r.appId = resolved;
      r.appDataRoot = appDataRootFor(r.appId);
    }
  }
  if (r.appId.empty() && looksLikeAppId(authorAppIdHint)) {
    r.appId = authorAppIdHint;
    r.appDataRoot = appDataRootFor(r.appId);
  }
  if (r.kind == Kind::ProviderNeedsFetch && !r.appId.empty()) {
    if (appInstalled && appInstalled(r.appId)) {
      r.overlayMessage = "opening provider…";
    }
  }
  return r;
}

inline Result resolveFromRecentBookFields(const std::string& recentPath, const std::string& originalSourcePath,
                                          const std::string& title, const std::string& authorAppIdHint,
                                          const std::function<bool(const std::string&)>& exists,
                                          const std::function<bool(const std::string&)>& appInstalled) {
  return resolveFromRecentBookFields(recentPath, originalSourcePath, title, authorAppIdHint, {}, exists,
                                     appInstalled);
}

inline std::string appHintForRecentBook(const std::string& recentPath, const std::string& originalSourcePath,
                                        const std::string& authorField,
                                        const ProviderAppIdResolver& appIdForProvider) {
  if (!M4ContentProvider::isHistoryUri(recentPath.c_str())) return originalSourcePath;
  if (originalSourcePath.compare(0, 4, "app:") == 0) return originalSourcePath;
  std::string ignoredAppId;
  if (!originalSourcePath.empty() && appIdFromAppsDataAbs(originalSourcePath, ignoredAppId)) {
    return originalSourcePath;
  }
  std::string providerId;
  std::string bookId;
  if (M4ContentProvider::parseHistoryUri(recentPath.c_str(), providerId, bookId) && appIdForProvider) {
    const std::string appId = appIdForProvider(providerId);
    if (looksLikeAppId(appId)) return std::string("app:") + appId;
  }
  if (looksLikeAppId(authorField)) return std::string("app:") + authorField;
  return originalSourcePath;
}

}  // namespace M4HistoryReopen

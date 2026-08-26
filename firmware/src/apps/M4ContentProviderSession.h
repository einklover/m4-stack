#pragma once

// Thread-safe runtime registry for provider-backed books (host-testable).
// The registry is provider/book keyed. It intentionally does not bind itself
// to one Lua app: native apps may hand off books and destroy their runtime,
// and another native provider may be opened without a process restart.

#include "util/M4ContentProviderContract.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace M4ContentProviderSession {

using M4ContentProvider::BookSpec;
using M4ContentProvider::ChapterMeta;
using M4ContentProvider::ChapterReady;
using M4ContentProvider::ChapterStatus;
using M4ContentProvider::HistoryEntry;
using M4ContentProvider::PrefetchWork;

struct BookState {
  BookSpec spec;
  std::unordered_map<int, ChapterStatus> chapters;
  size_t chapterCount = 0;
  int lastOpenIndex0 = 0;
  size_t lastByteOffset = 0;
  bool hasLastByteOffset = false;
  int lastPage0 = 0;
  int lastPageCount = 0;
  bool historyRegistered = false;
};

inline std::mutex& mu() {
  static std::mutex m;
  return m;
}

// Kept for source compatibility with older diagnostics. It is no longer used
// as an authorization gate; app identity belongs in BookSpec, not a global.
inline std::string& boundAppId() {
  static std::string id;
  return id;
}

inline std::unordered_map<std::string, BookState>& books() {
  static std::unordered_map<std::string, BookState> m;
  return m;
}

inline std::vector<PrefetchWork>& workQ() {
  static std::vector<PrefetchWork> q;
  return q;
}

inline std::string bookKey(const std::string& providerId, const std::string& bookId) {
  return providerId + "\n" + bookId;
}

inline void clearForApp(const std::string& appId) {
  std::lock_guard<std::mutex> lock(mu());
  // Old code assigned appId here and registerBook rejected a later different
  // app forever. Native app handoff makes that assumption invalid. Clear the
  // compatibility slot and remove only this app's states; an empty appId is
  // the explicit global-reset form used at boot/tests.
  boundAppId().clear();
  if (appId.empty()) {
    books().clear();
    workQ().clear();
    return;
  }
  for (auto it = books().begin(); it != books().end();) {
    if (it->second.spec.appId == appId) it = books().erase(it);
    else ++it;
  }
  auto& q = workQ();
  q.erase(std::remove_if(q.begin(), q.end(), [&](const PrefetchWork& w) {
            auto it = books().find(bookKey(w.providerId, w.bookId));
            return it == books().end();
          }), q.end());
}

inline bool registerBook(const BookSpec& in) {
  using namespace M4ContentProvider;
  if (validateBookSpec(in) != ValidationError::None) return false;

  std::lock_guard<std::mutex> lock(mu());
  const std::string key = bookKey(in.providerId, in.bookId);

  std::unordered_map<int, ChapterStatus> keepChapters;
  int keepLastOpen = 0;
  size_t keepByte = 0;
  bool keepHasByte = false;
  int keepPage0 = 0;
  int keepPageCount = 0;
  bool keepHistory = false;
  {
    auto it = books().find(key);
    if (it != books().end()) {
      // Never let another package silently re-bind an existing provider/book
      // identity. Same app or an empty legacy appId is allowed.
      if (!it->second.spec.appId.empty() && !in.appId.empty() &&
          it->second.spec.appId != in.appId) return false;
      keepChapters = std::move(it->second.chapters);
      keepLastOpen = it->second.lastOpenIndex0;
      keepByte = it->second.lastByteOffset;
      keepHasByte = it->second.hasLastByteOffset;
      keepPage0 = it->second.lastPage0;
      keepPageCount = it->second.lastPageCount;
      keepHistory = it->second.historyRegistered;
    }
  }

  BookState st;
  st.spec = in;
  st.chapterCount = bookChapterCount(in);
  for (size_t i = 0; i < in.chapters.size(); ++i) {
    ChapterStatus& cs = st.chapters[static_cast<int>(i)];
    cs.providerId = in.providerId;
    cs.bookId = in.bookId;
    cs.chapterUid = in.chapters[i].uid;
    cs.index0 = static_cast<int>(i);
    cs.state = ChapterReady::Missing;
    cs.pct = 0;
  }
  for (auto& kv : keepChapters) {
    const int idx = kv.first;
    if (idx < 0 || static_cast<size_t>(idx) >= st.chapterCount) continue;
    ChapterStatus& prev = kv.second;
    if (prev.state != ChapterReady::Ready && prev.state != ChapterReady::Fetching &&
        prev.state != ChapterReady::Error) continue;
    ChapterStatus& cs = st.chapters[idx];
    if (cs.chapterUid.empty() && !prev.chapterUid.empty()) cs.chapterUid = prev.chapterUid;
    if (!prev.chapterUid.empty() && (cs.chapterUid.empty() || cs.chapterUid == prev.chapterUid)) {
      cs = prev;
      cs.providerId = in.providerId;
      cs.bookId = in.bookId;
      cs.index0 = idx;
    } else if (cs.chapterUid.empty()) {
      cs = prev;
      cs.providerId = in.providerId;
      cs.bookId = in.bookId;
      cs.index0 = idx;
    }
  }
  if (in.currentIndex0 >= 0 && static_cast<size_t>(in.currentIndex0) < st.chapterCount) {
    st.lastOpenIndex0 = in.currentIndex0;
  } else if (keepLastOpen >= 0 && static_cast<size_t>(keepLastOpen) < st.chapterCount) {
    st.lastOpenIndex0 = keepLastOpen;
  }
  st.lastByteOffset = keepByte;
  st.hasLastByteOffset = keepHasByte;
  st.lastPage0 = keepPage0;
  st.lastPageCount = keepPageCount;
  st.historyRegistered = keepHistory;
  books()[key] = std::move(st);
  return true;
}

inline bool setChapterStatus(const ChapterStatus& in) {
  using namespace M4ContentProvider;
  if (!idOk(in.providerId.c_str(), kMaxProviderIdLen) || !idOk(in.bookId.c_str(), kMaxBookIdLen)) return false;
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(in.providerId, in.bookId));
  if (it == books().end()) return false;
  BookState& st = it->second;
  if (st.spec.catalog.kind == M4ContentProvider::ChapterCatalogKind::FileRows &&
      (in.state == ChapterReady::Fetching || in.state == ChapterReady::Ready) && in.chapterUid.empty()) {
    return false;
  }
  int idx = in.index0;
  if (idx < 0) {
    for (const auto& item : st.chapters) {
      if (item.second.chapterUid == in.chapterUid) {
        idx = item.first;
        break;
      }
    }
  }
  if (idx < 0 || static_cast<size_t>(idx) >= st.chapterCount) return false;
  // Validate before mutating: a rejected Ready must leave the previous status
  // untouched, otherwise UI polling sees a phantom Ready with no openable
  // cache path (host contract: test_fanqie_reader_handoff.cpp).
  if (in.state == ChapterReady::Ready && !isSafeCacheRelPath(in.cacheRelPath.c_str())) return false;
  ChapterStatus& cs = st.chapters[idx];
  if (cs.chapterUid.empty() && static_cast<size_t>(idx) < st.spec.chapters.size()) {
    cs.chapterUid = st.spec.chapters[static_cast<size_t>(idx)].uid;
  }
  cs.providerId = in.providerId;
  cs.bookId = in.bookId;
  cs.state = in.state;
  cs.pct = normalizePct(in.state, in.pct);
  cs.error = in.error;
  if (in.state == ChapterReady::Ready) {
    cs.cacheRelPath = in.cacheRelPath;
    cs.pct = 100;
    cs.error.clear();
  } else if (in.state == ChapterReady::Missing || in.state == ChapterReady::Error) {
    cs.cacheRelPath.clear();
  }
  if (!in.chapterUid.empty()) cs.chapterUid = in.chapterUid;
  cs.index0 = idx;
  if (in.state == ChapterReady::Ready) {
    auto& q = workQ();
    q.erase(std::remove_if(q.begin(), q.end(), [&](const PrefetchWork& w) {
              return w.providerId == in.providerId && w.bookId == in.bookId && w.index0 == idx;
            }), q.end());
  }
  return true;
}

struct HistoryResume {
  bool pending = false;
  std::string appId;
  std::string providerId;
  std::string bookId;
  std::string title;
  std::string chapterUid;
  std::string cacheRelPath;
  int chapterIndex0 = 0;
  size_t byteOffset = 0;
  bool hasByteOffset = false;
};

inline HistoryResume& historyResumeSlot() {
  static HistoryResume r;
  return r;
}

inline void queueHistoryResume(const HistoryResume& r) {
  std::lock_guard<std::mutex> lock(mu());
  historyResumeSlot() = r;
  historyResumeSlot().pending = true;
}

inline bool takeHistoryResume(HistoryResume& out) {
  std::lock_guard<std::mutex> lock(mu());
  if (!historyResumeSlot().pending) return false;
  out = historyResumeSlot();
  historyResumeSlot() = {};
  return true;
}

inline ChapterStatus chapterAt(const std::string& providerId, const std::string& bookId, int index0) {
  ChapterStatus out;
  out.providerId = providerId;
  out.bookId = bookId;
  out.index0 = index0;
  out.state = ChapterReady::Missing;
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end()) return out;
  const BookState& st = it->second;
  if (index0 < 0 || static_cast<size_t>(index0) >= st.chapterCount) return out;
  auto found = st.chapters.find(index0);
  if (found != st.chapters.end()) return found->second;
  if (static_cast<size_t>(index0) < st.spec.chapters.size()) out.chapterUid = st.spec.chapters[static_cast<size_t>(index0)].uid;
  return out;
}

inline bool requestPrefetch(const std::string& providerId, const std::string& bookId, int index0) {
  using namespace M4ContentProvider;
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end()) return false;
  BookState& st = it->second;
  if (index0 < 0 || static_cast<size_t>(index0) >= st.chapterCount) return false;
  ChapterStatus& cs = st.chapters[index0];
  cs.providerId = providerId;
  cs.bookId = bookId;
  cs.index0 = index0;
  if (cs.chapterUid.empty() && static_cast<size_t>(index0) < st.spec.chapters.size()) {
    cs.chapterUid = st.spec.chapters[static_cast<size_t>(index0)].uid;
  }
  if (cs.state == ChapterReady::Ready) return false;
  if (cs.state != ChapterReady::Fetching) {
    cs.state = ChapterReady::Fetching;
    cs.pct = std::max(cs.pct, 1);
  }
  for (const auto& w : workQ()) {
    if (w.providerId == providerId && w.bookId == bookId && w.index0 == index0) return true;
  }
  PrefetchWork w;
  w.valid = true;
  w.providerId = providerId;
  w.bookId = bookId;
  w.chapterUid = cs.chapterUid;
  w.index0 = index0;
  w.catalog = st.spec.catalog;
  workQ().push_back(std::move(w));
  return true;
}

inline PrefetchWork pollWork() {
  std::lock_guard<std::mutex> lock(mu());
  PrefetchWork out;
  if (workQ().empty()) return out;
  out = workQ().front();
  workQ().erase(workQ().begin());
  return out;
}

inline bool catalogFor(const std::string& providerId, const std::string& bookId, int index0,
                       M4ContentProvider::ChapterCatalogSpec& out) {
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end() || it->second.spec.catalog.kind != M4ContentProvider::ChapterCatalogKind::FileRows ||
      index0 < 0 || static_cast<size_t>(index0) >= it->second.chapterCount) return false;
  out = it->second.spec.catalog;
  return true;
}

inline void noteOpen(const std::string& providerId, const std::string& bookId, int index0, size_t byteOffset) {
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end()) return;
  if (index0 >= 0 && static_cast<size_t>(index0) < it->second.chapterCount) it->second.lastOpenIndex0 = index0;
  it->second.lastByteOffset = byteOffset;
  it->second.hasLastByteOffset = true;
}

inline bool noteProgress(const std::string& providerId, const std::string& bookId,
                         const M4ContentProvider::ReadPosition& position) {
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end()) return false;
  BookState& st = it->second;
  if (!M4ContentProvider::validReadPosition(position, st.chapterCount)) return false;
  auto found = st.chapters.find(position.chapterIndex0);
  if (found != st.chapters.end() && !position.chapterUid.empty() &&
      !found->second.chapterUid.empty() && position.chapterUid != found->second.chapterUid) return false;
  st.lastOpenIndex0 = position.chapterIndex0;
  st.lastByteOffset = position.byteOffset;
  st.hasLastByteOffset = position.hasByteOffset;
  st.lastPage0 = position.page0;
  st.lastPageCount = position.pageCount;
  return true;
}

inline HistoryEntry makeHistorySnapshot(const std::string& providerId, const std::string& bookId) {
  HistoryEntry h;
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end()) return h;
  const BookState& st = it->second;
  h.providerId = st.spec.providerId;
  h.bookId = st.spec.bookId;
  h.title = st.spec.title;
  h.chapterIndex0 = st.lastOpenIndex0;
  h.byteOffset = st.lastByteOffset;
  h.hasByteOffset = st.hasLastByteOffset;
  h.page0 = st.lastPage0;
  h.pageCount = st.lastPageCount;
  auto chapter = st.chapters.find(h.chapterIndex0);
  if (chapter != st.chapters.end()) {
    h.chapterUid = chapter->second.chapterUid;
    h.cacheRelPath = chapter->second.cacheRelPath;
  }
  return h;
}

inline bool resolveReopen(const std::string& providerId, const std::string& bookId, ChapterStatus& out) {
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end()) return false;
  const BookState& st = it->second;
  auto tryIdx = [&](int i) -> bool {
    if (i < 0 || static_cast<size_t>(i) >= st.chapterCount) return false;
    auto found = st.chapters.find(i);
    if (found == st.chapters.end()) return false;
    const ChapterStatus& cs = found->second;
    if (cs.state == ChapterReady::Ready && !cs.cacheRelPath.empty()) {
      out = cs;
      return true;
    }
    return false;
  };
  if (tryIdx(st.lastOpenIndex0)) return true;
  int earliest = -1;
  for (const auto& item : st.chapters) {
    const ChapterStatus& cs = item.second;
    if (cs.state == ChapterReady::Ready && !cs.cacheRelPath.empty() && (earliest < 0 || item.first < earliest)) {
      earliest = item.first;
    }
  }
  return earliest >= 0 && tryIdx(earliest);
}

inline bool markHistoryRegistered(const std::string& providerId, const std::string& bookId) {
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  if (it == books().end()) return false;
  it->second.historyRegistered = true;
  return true;
}

inline bool isHistoryRegistered(const std::string& providerId, const std::string& bookId) {
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  return it != books().end() && it->second.historyRegistered;
}

inline size_t bookCount() {
  std::lock_guard<std::mutex> lock(mu());
  return books().size();
}

inline size_t pendingWorkCount() {
  std::lock_guard<std::mutex> lock(mu());
  return workQ().size();
}

inline size_t chapterStatusCount(const std::string& providerId, const std::string& bookId) {
  std::lock_guard<std::mutex> lock(mu());
  auto it = books().find(bookKey(providerId, bookId));
  return it == books().end() ? 0 : it->second.chapters.size();
}

}  // namespace M4ContentProviderSession

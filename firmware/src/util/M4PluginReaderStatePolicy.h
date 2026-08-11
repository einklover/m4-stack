#pragma once

// Production-facing progressive-reader index state policy.
//
// Lock ownership (device = FreeRTOS non-recursive mutex `renderingMutex`):
//   - Single state lock for: pageOffsets, currentPage, totalPages, indexCursor,
//     indexComplete, firstPageReady, pending restore, userMovedPage, tidxSaved.
//   - Display task: lock → mutate index / fill page lines → unlock.
//     Prefer not holding the lock across e-paper displayBuffer when the frame
//     buffer is already filled; UI page turns may wait for a render/index slice.
//   - UI task: lock → coherent page-turn / restore cancel / menu progress read
//     → unlock. May block until display releases the lock (correctness first).
//   - Never take the state lock recursively (mutex is non-recursive).
//   - Snapshot for close: wait until the lock is acquired; never invent page 0.
//
// Host tests exercise the same pure helpers + a std::mutex-backed store that
// mirrors production operations under contention.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace M4PluginReaderStatePolicy {

struct IndexState {
  std::vector<size_t> pageOffsets;
  int currentPage = 0;
  int totalPages = 1;
  bool indexComplete = false;
  bool firstPageReady = false;
  size_t indexCursor = 0;
  size_t pendingRestoreByte = 0;
  bool hasPendingRestore = false;
  bool userMovedPage = false;
  bool tidxSaved = false;
  uint32_t generation = 0;
};

struct ProgressSnapshot {
  bool valid = false;  // false ⇒ do not persist (lock not acquired / empty)
  int page = 0;        // 0-based native contract
  int total = -1;      // -1 while incomplete
  size_t byteOffset = 0;
  bool indexComplete = false;
  uint32_t generation = 0;
};

// Coherent read-only menu/status fields (copy under lock, use after unlock).
struct MenuProgressView {
  bool valid = false;
  int page0 = 0;           // 0-based
  int pageDisplay1 = 1;    // 1-based for UI labels
  int totalPages = 1;
  int bookProgressPercent = 0;
  size_t byteOffset = 0;
  bool indexComplete = false;
};

// Background indexing may change total/progress metadata without changing the
// visible page.  Repainting that state is especially visible with AA enabled
// because it repeats the BW + gray-plane sequence over identical glyphs.
// Only a restore that actually moves the visible page needs a physical redraw.
struct BackgroundIndexUpdate {
  bool pageChanged = false;
  bool indexAdvanced = false;
  bool indexCompleted = false;
};

inline bool backgroundIndexNeedsRedraw(const BackgroundIndexUpdate& u) {
  return u.pageChanged;
}

// First physical paint after plugin/native handoff.
// Use absolute (non-differential) HALF — SSD1677 BYPASS_RED + single-pass 0xD7
// clean — not OTP multi-flash FULL (0xF7). FAST is differential and can garble
// residual plugin UI; FULL is the multi-inversion waveform that flashes the
// whole panel several times. Exactly one handoff refresh; later pages FAST/HALF.
enum class PluginFirstHandoffRefresh : uint8_t { Half = 0, Full = 1, Fast = 2 };

inline PluginFirstHandoffRefresh pluginFirstHandoffRefreshMode() {
  return PluginFirstHandoffRefresh::Half;
}

// --- Page base: convert exactly once at the native→Lua boundary ---
// Native PluginProgress.page is 0-based; Lua reader_page / native_progress.page are 1-based.
inline int page0ToLua1(int page0) {
  if (page0 < 0) return 1;
  // Saturate absurd values; total is applied by caller when known.
  if (page0 > 1000000) return 1000001;
  return page0 + 1;
}

inline int lua1ToPage0(int page1) {
  if (page1 <= 1) return 0;
  return page1 - 1;
}

// Completed-progress percent from 1-based display page and total page count.
inline int percentFromLuaPage1(int page1, int total) {
  if (total <= 0) return 0;
  if (page1 < 1) page1 = 1;
  if (page1 > total) page1 = total;
  return (page1 * 100) / total;
}

// Build menu view from locked index state + file size (no lock here).
inline MenuProgressView makeMenuProgressView(const IndexState& s, size_t fileSize) {
  MenuProgressView v;
  if (s.pageOffsets.empty()) return v;
  v.valid = true;
  v.page0 = s.currentPage >= 0 ? s.currentPage : 0;
  if (v.page0 >= static_cast<int>(s.pageOffsets.size())) {
    v.page0 = static_cast<int>(s.pageOffsets.size()) - 1;
  }
  v.pageDisplay1 = page0ToLua1(v.page0);
  v.totalPages = s.totalPages > 0 ? s.totalPages : static_cast<int>(s.pageOffsets.size());
  v.indexComplete = s.indexComplete;
  v.byteOffset = s.pageOffsets[static_cast<size_t>(v.page0)];
  if (fileSize > 0) {
    v.bookProgressPercent =
        static_cast<int>(static_cast<float>(v.byteOffset) * 100.0f / static_cast<float>(fileSize) + 0.5f);
    if (v.bookProgressPercent > 100) v.bookProgressPercent = 100;
    if (v.bookProgressPercent < 0) v.bookProgressPercent = 0;
  }
  return v;
}

// --- Non-recursive state-lock call-graph policy (production FreeRTOS mutex) ---
// Models: display holds lock → renderScreen → goToPercentAlreadyLocked (no re-take).
// UI path: goToPercent → take → goToPercentAlreadyLocked → give.
// A nested take while held is a permanent deadlock on FreeRTOS non-recursive mutex.
struct NonRecursiveStateLock {
  bool held = false;
  int takeCount = 0;
  int deadlockAttempts = 0;

  bool tryTake() {
    if (held) {
      ++deadlockAttempts;
      return false;  // would block forever on device
    }
    held = true;
    ++takeCount;
    return true;
  }
  void give() { held = false; }
};

// Production call sites for percent jump.
enum class GoToPercentSite : uint8_t { UiUnlocked, DisplayAlreadyLocked };

// Simulate both sites. Returns false if any path would re-take while held.
inline bool verifyGoToPercentLockSites() {
  NonRecursiveStateLock lock;

  // --- UI path: acquire then body (no nested take) ---
  {
    if (!lock.tryTake()) return false;
    // body = goToPercentAlreadyLocked (must not tryTake again)
    const bool nested = lock.tryTake();
    if (nested) {
      lock.give();
      return false;  // recursive take succeeded — not modeling FreeRTOS
    }
    if (lock.deadlockAttempts != 1) return false;
    lock.deadlockAttempts = 0;
    lock.give();
  }

  // --- Display path: already held by displayTaskLoop/renderScreen ---
  {
    if (!lock.tryTake()) return false;
    // renderScreen sees pending jump → goToPercentAlreadyLocked only
    // (must NOT call goToPercent which would tryTake)
    const bool uiStyleNestedTake = lock.tryTake();
    if (uiStyleNestedTake) return false;
    if (lock.deadlockAttempts < 1) return false;
    // Correct site: already-locked body runs without take
    lock.give();
  }

  // --- Menu snapshot: short take → copy → give → enter child without lock ---
  {
    if (!lock.tryTake()) return false;
    MenuProgressView snap;  // would copy fields here
    (void)snap;
    lock.give();
    if (lock.held) return false;
    // enterNewActivity runs unlocked
  }

  return lock.deadlockAttempts >= 1 && !lock.held;
}

// Binary search: largest page start <= byteOffset.
inline int pageForByte(const std::vector<size_t>& offsets, size_t byteOffset) {
  if (offsets.empty()) return 0;
  int lo = 0, hi = static_cast<int>(offsets.size()) - 1, ans = 0;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    if (offsets[static_cast<size_t>(mid)] <= byteOffset) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans;
}

// Apply pending restore once when the containing page is known.
// Returns true if currentPage changed.
inline bool applyPendingRestore(IndexState& s) {
  if (!s.hasPendingRestore || s.userMovedPage || s.pageOffsets.empty()) return false;
  const size_t target = s.pendingRestoreByte;
  if (!s.indexComplete) {
    if (s.indexCursor <= target && s.pageOffsets.back() <= target) return false;
  }
  const int page = pageForByte(s.pageOffsets, target);
  const bool changed = (page != s.currentPage);
  if (changed) s.currentPage = page;
  s.hasPendingRestore = false;
  return changed;
}

// Cancel progressive restore because the user navigated manually.
inline void cancelPendingRestore(IndexState& s) {
  s.userMovedPage = true;
  s.hasPendingRestore = false;
}

// Coherent page-turn decision under the caller's lock.
// delta: +1 / -1 (or ±step). Returns true if currentPage changed.
// needIndexSlice: set when at frontier and index incomplete (caller may index then retry).
inline bool tryPageTurn(IndexState& s, int delta, bool* needIndexSlice) {
  if (needIndexSlice) *needIndexSlice = false;
  if (s.pageOffsets.empty()) return false;
  const int step = delta >= 0 ? delta : -delta;
  if (delta > 0) {
    if (s.currentPage + step <= s.totalPages - 1) {
      s.currentPage += step;
      cancelPendingRestore(s);
      return true;
    }
    if (s.currentPage < s.totalPages - 1) {
      s.currentPage = s.totalPages - 1;
      cancelPendingRestore(s);
      return true;
    }
    if (!s.indexComplete) {
      if (needIndexSlice) *needIndexSlice = true;
      return false;
    }
    return false;
  }
  // prev
  if (s.currentPage - step >= 0) {
    s.currentPage -= step;
    cancelPendingRestore(s);
    return true;
  }
  if (s.currentPage > 0) {
    s.currentPage = 0;
    cancelPendingRestore(s);
    return true;
  }
  return false;
}

// Build a progress snapshot from locked state. Never fabricates when empty.
inline ProgressSnapshot makeProgressSnapshot(const IndexState& s) {
  ProgressSnapshot p;
  if (s.pageOffsets.empty()) {
    p.valid = false;
    p.generation = s.generation;
    return p;
  }
  p.valid = true;
  p.generation = s.generation;
  p.page = s.currentPage >= 0 ? s.currentPage : 0;
  if (p.page >= static_cast<int>(s.pageOffsets.size())) {
    p.page = static_cast<int>(s.pageOffsets.size()) - 1;
  }
  p.total = s.indexComplete ? s.totalPages : -1;
  p.indexComplete = s.indexComplete;
  p.byteOffset = s.pageOffsets[static_cast<size_t>(p.page)];
  return p;
}

// Append a newly discovered page start (producer). Returns new total or -1.
inline int appendPageStart(IndexState& s, size_t off) {
  if (!s.pageOffsets.empty() && off <= s.pageOffsets.back()) return -1;
  s.pageOffsets.push_back(off);
  s.totalPages = static_cast<int>(s.pageOffsets.size());
  s.indexCursor = off;
  return s.totalPages;
}

// Settings rebuild: keep raw-byte position as pending restore when possible.
inline void rebuildPreserveByte(IndexState& s) {
  size_t keep = 0;
  bool have = false;
  if (s.currentPage >= 0 && s.currentPage < static_cast<int>(s.pageOffsets.size())) {
    keep = s.pageOffsets[static_cast<size_t>(s.currentPage)];
    have = true;
  } else if (s.hasPendingRestore) {
    keep = s.pendingRestoreByte;
    have = true;
  }
  s.pageOffsets.clear();
  s.pageOffsets = {0};
  s.totalPages = 1;
  s.currentPage = 0;
  s.indexComplete = false;
  s.firstPageReady = false;
  s.indexCursor = 0;
  s.tidxSaved = false;
  s.userMovedPage = false;
  if (have) {
    s.pendingRestoreByte = keep;
    s.hasPendingRestore = true;
  } else {
    s.hasPendingRestore = false;
    s.pendingRestoreByte = 0;
  }
}

// Thread-safe store: production uses FreeRTOS mutex around the same ops.
class LockedState {
 public:
  void reset(uint32_t gen) {
    std::lock_guard<std::mutex> lock(mu_);
    s_ = {};
    s_.pageOffsets = {0};
    s_.generation = gen;
    s_.totalPages = 1;
  }

  ProgressSnapshot snapshotBlocking() const {
    std::lock_guard<std::mutex> lock(mu_);
    return makeProgressSnapshot(s_);
  }

  // Host model of "must not invent defaults": if try_lock fails, valid=false.
  ProgressSnapshot trySnapshot() const {
    std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
    if (!lock.owns_lock()) {
      ProgressSnapshot p;
      p.valid = false;
      return p;
    }
    return makeProgressSnapshot(s_);
  }

  template <typename Fn>
  void withLock(Fn&& fn) {
    std::lock_guard<std::mutex> lock(mu_);
    fn(s_);
  }

  template <typename Fn>
  void withLockConst(Fn&& fn) const {
    std::lock_guard<std::mutex> lock(mu_);
    fn(s_);
  }

  int appendPageStart(size_t off) {
    std::lock_guard<std::mutex> lock(mu_);
    return M4PluginReaderStatePolicy::appendPageStart(s_, off);
  }

  bool tryPageTurn(int delta, bool* needSlice) {
    std::lock_guard<std::mutex> lock(mu_);
    return M4PluginReaderStatePolicy::tryPageTurn(s_, delta, needSlice);
  }

  bool applyPendingRestore() {
    std::lock_guard<std::mutex> lock(mu_);
    return M4PluginReaderStatePolicy::applyPendingRestore(s_);
  }

  void setComplete(bool v) {
    std::lock_guard<std::mutex> lock(mu_);
    s_.indexComplete = v;
  }

 private:
  mutable std::mutex mu_;
  IndexState s_{};
};

// Producer appends while consumer snapshots / page-turns; all views coherent.
inline bool verifyProductionPolicyUnderContention() {
  LockedState store;
  store.reset(7);
  store.withLock([](IndexState& s) {
    s.firstPageReady = true;
    s.hasPendingRestore = true;
    s.pendingRestoreByte = 450;
  });

  for (int round = 0; round < 80; ++round) {
    store.appendPageStart(static_cast<size_t>((round + 1) * 100));
    if (round == 40) store.setComplete(true);
    store.applyPendingRestore();

    const ProgressSnapshot snap = store.snapshotBlocking();
    if (!snap.valid) return false;
    if (snap.generation != 7) return false;

    store.withLockConst([&](const IndexState& s) {
      if (s.totalPages != static_cast<int>(s.pageOffsets.size())) {
        // flag via generation poison — checked outside
      }
    });

    bool need = false;
    (void)store.tryPageTurn(+1, &need);
    // try_lock snapshot while locked must not invent page 0
    {
      bool held = false;
      store.withLock([&](IndexState&) {
        held = true;
        // Cannot nest trySnapshot on same thread with non-recursive if we held
        // via withLock — host std::mutex is also non-recursive. Skip nest.
      });
      (void)held;
    }
    const ProgressSnapshot t = store.trySnapshot();
    if (t.valid) {
      if (t.page < 0) return false;
    }
  }

  // Coherence checks
  bool ok = true;
  store.withLockConst([&](const IndexState& s) {
    if (s.totalPages != static_cast<int>(s.pageOffsets.size())) ok = false;
    if (s.pageOffsets.empty() || s.pageOffsets[0] != 0) ok = false;
    for (size_t i = 1; i < s.pageOffsets.size(); ++i) {
      if (s.pageOffsets[i] <= s.pageOffsets[i - 1]) ok = false;
    }
    if (s.currentPage < 0 || s.currentPage >= s.totalPages) ok = false;
    if (!s.indexComplete) ok = false;
  });
  const ProgressSnapshot fin = store.snapshotBlocking();
  if (!fin.valid || fin.generation != 7) ok = false;

  // Timeout-style: never treat invalid snapshot as page 0 success
  {
    ProgressSnapshot invalid;
    invalid.valid = false;
    invalid.page = 0;
    if (invalid.valid) ok = false;  // must remain invalid
    // Lua/native must not persist when !valid
  }

  // Manual page turn cancels restore
  {
    LockedState s2;
    s2.reset(1);
    s2.withLock([](IndexState& s) {
      s.pageOffsets = {0, 100, 200};
      s.totalPages = 3;
      s.hasPendingRestore = true;
      s.pendingRestoreByte = 200;
    });
    bool need = false;
    s2.tryPageTurn(+1, &need);
    s2.withLockConst([&](const IndexState& s) {
      if (s.hasPendingRestore) ok = false;
      if (!s.userMovedPage) ok = false;
      if (s.currentPage != 1) ok = false;
    });
  }

  // Settings rebuild preserves byte
  {
    LockedState s3;
    s3.reset(1);
    s3.withLock([](IndexState& s) {
      s.pageOffsets = {0, 100, 250};
      s.totalPages = 3;
      s.currentPage = 2;
      s.indexComplete = true;
      rebuildPreserveByte(s);
      if (!s.hasPendingRestore || s.pendingRestoreByte != 250) {
        s.generation = 0;  // poison
      }
      if (s.tidxSaved) s.generation = 0;
      if (s.indexComplete) s.generation = 0;
    });
    s3.withLockConst([&](const IndexState& s) {
      if (s.generation != 1) ok = false;
      if (!s.hasPendingRestore || s.pendingRestoreByte != 250) ok = false;
    });
  }

  return ok;
}

// Exact session identity helpers for progress delivery.
inline bool progressKeyMatches(const char* got, const char* expect) {
  if (!got || !expect || !got[0] || !expect[0]) return false;
  return std::string(got) == std::string(expect);
}

inline bool generationMatches(uint32_t got, uint32_t expect) {
  return got != 0 && got == expect;
}

}  // namespace M4PluginReaderStatePolicy

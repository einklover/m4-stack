#pragma once
// Shared book indexing / open lifecycle for Murphy M4 firmware + host/simulator.
// UI and firmware must leave busy state on every terminal path.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace M4IndexingState {

enum class Phase : uint8_t {
  Idle = 0,
  Preparing,
  Indexing,
  Ready,
  Failed,
  Cancelled,
};

struct Snapshot {
  Phase phase = Phase::Idle;
  int progressPct = 0;      // 0..100 while Indexing
  uint32_t itemCount = 0;   // pages/spines/bytes processed
  uint32_t itemTotal = 0;   // 0 if unknown
  uint32_t elapsedMs = 0;
  uint32_t lastProgressMs = 0;
  uint32_t freeHeapHint = 0;  // optional observer field (0 if unknown)
  char message[96] = {};
  bool busy = false;  // true only for Preparing/Indexing
};

inline const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Preparing: return "preparing";
    case Phase::Indexing: return "indexing";
    case Phase::Ready: return "ready";
    case Phase::Failed: return "failed";
    case Phase::Cancelled: return "cancelled";
  }
  return "unknown";
}

inline bool isTerminal(Phase p) {
  return p == Phase::Ready || p == Phase::Failed || p == Phase::Cancelled || p == Phase::Idle;
}

inline bool isBusy(Phase p) { return p == Phase::Preparing || p == Phase::Indexing; }

class Machine {
 public:
  const Snapshot& snap() const { return s_; }

  void reset() { s_ = Snapshot{}; }

  void beginPreparing(const char* msg = nullptr) {
    s_.phase = Phase::Preparing;
    s_.busy = true;
    s_.progressPct = 0;
    s_.itemCount = 0;
    s_.itemTotal = 0;
    s_.elapsedMs = 0;
    s_.lastProgressMs = 0;
    s_.freeHeapHint = 0;
    setMsg(msg ? msg : "preparing");
  }

  void beginIndexing(uint32_t totalHint = 0, const char* msg = nullptr) {
    s_.phase = Phase::Indexing;
    s_.busy = true;
    s_.itemTotal = totalHint;
    if (s_.progressPct < 0) s_.progressPct = 0;
    setMsg(msg ? msg : "indexing");
  }

  // Progress may not go backwards. Returns false if ignored as non-monotonic.
  bool reportProgress(int pct, uint32_t items, uint32_t nowMs, const char* msg = nullptr,
                      uint32_t freeHeap = 0) {
    if (s_.phase != Phase::Indexing && s_.phase != Phase::Preparing) return false;
    if (s_.phase == Phase::Preparing) s_.phase = Phase::Indexing;
    s_.busy = true;
    if (pct < s_.progressPct) return false;
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    s_.progressPct = pct;
    s_.itemCount = items;
    s_.elapsedMs = nowMs;
    s_.lastProgressMs = nowMs;
    if (freeHeap) s_.freeHeapHint = freeHeap;
    if (msg) setMsg(msg);
    return true;
  }

  void succeed(uint32_t nowMs, const char* msg = nullptr) {
    s_.phase = Phase::Ready;
    s_.busy = false;
    s_.progressPct = 100;
    s_.elapsedMs = nowMs;
    setMsg(msg ? msg : "ready");
  }

  void fail(uint32_t nowMs, const char* msg) {
    s_.phase = Phase::Failed;
    s_.busy = false;
    s_.elapsedMs = nowMs;
    setMsg(msg ? msg : "failed");
  }

  void cancel(uint32_t nowMs, const char* msg = nullptr) {
    s_.phase = Phase::Cancelled;
    s_.busy = false;
    s_.elapsedMs = nowMs;
    setMsg(msg ? msg : "cancelled");
  }

  // Stale/partial cache must not be treated as complete.
  static bool acceptCacheAsComplete(bool fileExists, bool headerValid, bool spineCountPlausible,
                                    bool structureComplete) {
    return fileExists && headerValid && spineCountPlausible && structureComplete;
  }

  // Lack of progress: no progress event for stallMs while busy.
  bool stalled(uint32_t nowMs, uint32_t stallMs) const {
    if (!s_.busy) return false;
    if (s_.lastProgressMs == 0) return nowMs >= stallMs;
    return (nowMs - s_.lastProgressMs) >= stallMs;
  }

 private:
  void setMsg(const char* msg) {
    if (!msg) {
      s_.message[0] = 0;
      return;
    }
    std::strncpy(s_.message, msg, sizeof(s_.message) - 1);
    s_.message[sizeof(s_.message) - 1] = 0;
  }

  Snapshot s_;
};

// Process-wide session shared by firmware, host tests, and simulator.
class Session {
 public:
  static Session& get() {
    static Session inst;
    return inst;
  }

  Machine& machine() { return m_; }
  const Machine& machine() const { return m_; }
  const Snapshot& snap() const { return m_.snap(); }

  void reset() { m_.reset(); }

 private:
  Session() = default;
  Machine m_;
};

// RAII: if scope exits while still busy, force Failed (unless cancel requested).
// Call markSucceeded()/markCancelled() before leaving on intentional terminals.
class ScopeGuard {
 public:
  explicit ScopeGuard(const char* defaultFailMsg = "aborted", uint32_t (*nowFn)() = nullptr)
      : failMsg_(defaultFailMsg), nowFn_(nowFn), active_(true) {}

  ~ScopeGuard() {
    if (!active_) return;
    auto& m = Session::get().machine();
    if (m.snap().busy) {
      const uint32_t now = nowFn_ ? nowFn_() : m.snap().elapsedMs;
      m.fail(now, failMsg_);
    }
  }

  void markSucceeded() { active_ = false; }
  void markCancelled() { active_ = false; }
  void dismiss() { active_ = false; }  // when ownership transferred

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

 private:
  const char* failMsg_;
  uint32_t (*nowFn_)();
  bool active_;
};

// Throttle policy for e-paper: do not refresh every item.
inline bool shouldRefreshProgressUi(int lastShownPct, int newPct, int minDeltaPct = 5) {
  if (newPct >= 100) return true;
  return (newPct - lastShownPct) >= minDeltaPct;
}

// Serial line for [M4-INDEX] audits (host may capture; firmware Serial.printf).
inline std::string formatLogLine(const Snapshot& s) {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "[M4-INDEX] phase=%s progress=%d items=%u/%u elapsed=%u busy=%d heap=%u msg=%s",
                phaseName(s.phase), s.progressPct, static_cast<unsigned>(s.itemCount),
                static_cast<unsigned>(s.itemTotal), static_cast<unsigned>(s.elapsedMs), s.busy ? 1 : 0,
                static_cast<unsigned>(s.freeHeapHint), s.message);
  return std::string(buf);
}

}  // namespace M4IndexingState

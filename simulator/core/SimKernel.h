// M4Sim core: deterministic virtual scheduler + clock + unified event trace.
// Everything here is single-threaded and time-deterministic: events at the same
// virtual timestamp are ordered by a seed-derived hash so `--seed N` genuinely
// reorders same-time races (the point of schedule fuzzing).
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace m4sim {

class SimScheduler {
public:
  explicit SimScheduler(uint32_t seed = 0x5eed) : seed_(seed) {}

  uint32_t now() const { return now_; }
  uint32_t seed() const { return seed_; }

  // Run a callback at an absolute virtual time.
  void scheduleAt(uint32_t t, std::function<void()> fn) {
    queue_.push(Event{t, nextSeq_++, seedHash(t, nextSeq_ - 1, seed_) , std::move(fn)});
  }
  void scheduleIn(uint32_t ms, std::function<void()> fn) {
    scheduleAt(now_ + ms, std::move(fn));
  }

  // Repeating callback; reschedules itself every periodMs.
  void every(uint32_t periodMs, std::function<void()> fn) {
    auto self = std::make_shared<std::function<void()>>();
    *self = [this, periodMs, fn, self]() {
      scheduleAt(now_ + periodMs, *self);
      fn();
    };
    scheduleAt(now_ + periodMs, *self);
  }

  // Drain all events with time <= untilMs. Returns final virtual time.
  uint32_t runUntil(uint32_t untilMs) {
    while (!queue_.empty() && queue_.top().t <= untilMs) {
      Event e = queue_.top();
      queue_.pop();
      if (!e.fn) {
        fprintf(stderr, "FATAL: empty callback at t=%u (seed=%u)\n", e.t, seed_);
        abort();
      }
      now_ = e.t;
      e.fn();
      // SAFETY: run every invariant check after every event. A transient state
      // that self-heals before the next poll must still fail a `never` check.
      if (eventHook_) eventHook_(now_);
    }
    now_ = untilMs;
    return now_;
  }
  uint32_t runFor(uint32_t ms) { return runUntil(now_ + ms); }

  // Called after every event completes. Used for event-driven safety checks.
  void setEventHook(std::function<void(uint32_t)> hook) { eventHook_ = std::move(hook); }

private:
  struct Event {
    uint32_t t;
    uint64_t seq;
    uint64_t tieKey;  // seeded hash of seq: same-time ordering depends on seed
    std::function<void()> fn;
  };
  struct Cmp {
    bool operator()(const Event& a, const Event& b) const {
      if (a.t != b.t) return a.t > b.t;
      if (a.tieKey != b.tieKey) return a.tieKey > b.tieKey;
      return a.seq > b.seq;
    }
  };
  static uint64_t seedHash(uint32_t t, uint64_t seq, uint32_t seed) {
    // Murmur3-ish mix: deterministic per (t, seq, seed).
    uint64_t h = seq * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)t * 0xC2B2AE3D27D4EB4Full;
    h ^= (uint64_t)seed * 0x165667B19E3779F9ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
  }

  std::priority_queue<Event, std::vector<Event>, Cmp> queue_;
  uint32_t now_ = 0;
  uint32_t seed_;
  uint64_t nextSeq_ = 1;
  std::function<void(uint32_t)> eventHook_;
};

// Unified event log — the "soul" of the system. Every failure path dumps this.
enum class SimEventType {
  INPUT,             // user button / touch
  PAGE_TARGET,       // currentPage intent advanced
  INDEX_STARTED,
  INDEX_READY,
  FRAME_RENDERED,    // renderer finished a framebuffer (provenance known)
  EPD_SUBMITTED,     // framebuffer handed to SSD1677
  EPD_BUSY,
  EPD_COMMITTED,     // BUSY finished; physical panel == pending frame
  STATE_CHANGED,     // firmware state mutation (e.g. lastPhysicalBodyPage_)
  ALLOC,
  FREE,
  OOM,
  CONTRACT_VIOLATION,
  SD_READ,
  TLS_BEGIN,
  TLS_END,
  HTTP_CHUNK,
  ASSERT,
};

struct SimEvent {
  uint32_t t;
  SimEventType type;
  std::string msg;
};

class SimTrace {
public:
  void emit(SimEventType type, const std::string& msg, uint32_t t) {
    events_.push_back(SimEvent{t, type, msg});
  }
  const std::vector<SimEvent>& events() const { return events_; }

  std::string renderTimeline() const {
    std::string out;
    for (const auto& e : events_) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%04u ", e.t % 10000);
      out += buf;
      out += typeName(e.type);
      out += " ";
      out += e.msg;
      out += "\n";
    }
    return out;
  }

  // FNV-1a hash of the full event stream (t, type, msg). Two runs of the same
  // scenario with the same seed MUST produce identical hashes — this is real
  // deterministic replay, stronger than comparing the final failure vector.
  uint64_t hash() const {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    for (const auto& e : events_) {
      for (int i = 0; i < 4; ++i) mix((uint8_t)(e.t >> (i * 8)));
      for (int i = 0; i < 4; ++i) mix((uint8_t)((uint32_t)e.type >> (i * 8)));
      for (char c : e.msg) mix((uint8_t)c);
      mix(0xFF);
    }
    return h;
  }

  static const char* typeName(SimEventType t) {
    switch (t) {
      case SimEventType::INPUT: return "INPUT";
      case SimEventType::PAGE_TARGET: return "PAGE_TARGET";
      case SimEventType::INDEX_STARTED: return "INDEX_STARTED";
      case SimEventType::INDEX_READY: return "INDEX_READY";
      case SimEventType::FRAME_RENDERED: return "FRAME_RENDERED";
      case SimEventType::EPD_SUBMITTED: return "EPD_SUBMITTED";
      case SimEventType::EPD_BUSY: return "EPD_BUSY";
      case SimEventType::EPD_COMMITTED: return "EPD_COMMITTED";
      case SimEventType::STATE_CHANGED: return "STATE_CHANGED";
      case SimEventType::ALLOC: return "ALLOC";
      case SimEventType::FREE: return "FREE";
      case SimEventType::OOM: return "OOM";
      case SimEventType::CONTRACT_VIOLATION: return "CONTRACT_VIOLATION";
      case SimEventType::SD_READ: return "SD_READ";
      case SimEventType::TLS_BEGIN: return "TLS_BEGIN";
      case SimEventType::TLS_END: return "TLS_END";
      case SimEventType::HTTP_CHUNK: return "HTTP_CHUNK";
      case SimEventType::ASSERT: return "ASSERT";
    }
    return "?";
  }

private:
  std::vector<SimEvent> events_;
};

// Temporal assertions. `eventually`/`after` are monitored every 5ms of virtual
// time; `never` is checked at the end (a persistent divergence is what matters).
class SimAssertions {
public:
  struct Eventually {
    std::string label;
    std::function<bool()> pred;
    uint32_t deadlineMs;
    bool everTrue = false;
    bool failed = false;
  };
  struct Never {
    std::string label;
    std::function<bool()> pred;
    bool failed = false;
  };
  struct After {
    std::string label;
    std::function<bool()> trigger;
    std::function<bool()> mustHappen;
    uint32_t deadlineMs;
    bool armed = false;
    uint32_t armedAt = 0;
    bool ok = false;
    bool failed = false;
  };

  void eventually(const std::string& label, std::function<bool()> pred, uint32_t deadlineMs) {
    eventually_.push_back(Eventually{label, std::move(pred), deadlineMs});
  }
  void never(const std::string& label, std::function<bool()> pred) {
    never_.push_back(Never{label, std::move(pred)});
  }
  void after(const std::string& label, std::function<bool()> trigger,
             std::function<bool()> mustHappen, uint32_t deadlineMs) {
    after_.push_back(After{label, std::move(trigger), std::move(mustHappen), deadlineMs});
  }

  // Event-driven safety check: runs immediately AFTER every scheduler event.
  // `never` invariants are true safety properties — a single instant of
  // violation must fail the test, even if the state self-heals before the next
  // poll. This is why polling every 5ms was wrong (divergence between polls is
  // invisible).
  void checkSafety(uint32_t now) {
    for (auto& a : never_) {
      if (!a.failed && a.pred()) {
        a.failed = true;
        failures_.push_back("NEVER failed at t=" + std::to_string(now) + "ms: " + a.label);
      }
    }
  }

  // Event-driven liveness: `eventually` records the first time it became true
  // and fails at deadline; `after` arms on trigger and fails if not satisfied.
  void checkLiveness(uint32_t now) {
    for (auto& a : eventually_) {
      if (a.pred()) a.everTrue = true;
      if (!a.everTrue && now >= a.deadlineMs && !a.failed) {
        a.failed = true;
        failures_.push_back("EVENTUALLY failed: " + a.label + " (deadline " +
                            std::to_string(a.deadlineMs) + "ms, now " + std::to_string(now) + "ms)");
      }
    }
    for (auto& a : after_) {
      if (!a.armed && a.trigger()) {
        a.armed = true;
        a.armedAt = now;
      }
      if (a.armed && !a.ok && a.mustHappen()) a.ok = true;
      if (a.armed && !a.ok && !a.failed && now >= a.armedAt + a.deadlineMs) {
        a.failed = true;
        failures_.push_back("AFTER failed: " + a.label);
      }
    }
  }

  // Legacy polling API kept for compatibility; checkLiveness is the real work.
  void poll(uint32_t now) { checkLiveness(now); }

  // Call at end of scenario.
  void finish(uint32_t now) {
    for (auto& a : eventually_) {
      if (a.everTrue) continue;
      if (!a.failed)
        failures_.push_back("EVENTUALLY failed: " + a.label);
    }
    checkSafety(now);
    checkLiveness(now);
  }

  const std::vector<std::string>& failures() const { return failures_; }
  bool ok() const { return failures_.empty(); }

private:
  std::vector<Eventually> eventually_;
  std::vector<Never> never_;
  std::vector<After> after_;
  std::vector<std::string> failures_;
};

}  // namespace m4sim

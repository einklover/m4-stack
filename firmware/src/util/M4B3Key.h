#pragma once

// Host-testable M4B3 Browser Bridge key-return queue.
//
// The main firmware loop is the producer (physical GPIO -> logical Browser
// action). The M4B3 receiver task is the only socket writer and drains this
// bounded queue. FRAME_ACK and panel presentation never depend on key TX.

#include <cstddef>
#include <cstdint>

#include "util/M4B3Protocol.h"

namespace M4B3Key {

constexpr size_t kQueueDepth = 8;

inline bool validAction(uint8_t action) { return M4B3::validInputKeyAction(action); }

struct Event {
  uint8_t action = 0;
  uint8_t flags = 0;
  uint32_t tMs = 0;
  uint32_t seq = 0;
  uint32_t session = 0;
};

struct Stats {
  uint32_t back = 0;
  uint32_t reload = 0;
  uint32_t rejected = 0;
  uint32_t overflow = 0;
  uint32_t sessionResets = 0;
};

enum class PushResult : uint8_t {
  Ok = 0,
  Rejected,
  Overflow,
};

class Queue {
 public:
  void resetSession() {
    head_ = 0;
    count_ = 0;
    nextSeq_ = 0;
    session_++;
    stats_.sessionResets++;
  }

  uint32_t session() const { return session_; }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  uint32_t lastSeq() const { return lastSeq_; }
  uint8_t lastAction() const { return lastAction_; }
  const Stats& stats() const { return stats_; }
  Stats& stats() { return stats_; }

  PushResult push(uint8_t action, uint32_t tMs, uint8_t flags = 0) {
    if (!validAction(action)) {
      stats_.rejected++;
      return PushResult::Rejected;
    }
    if (count_ == kQueueDepth) {
      stats_.overflow++;
      return PushResult::Overflow;
    }
    const size_t tail = (static_cast<size_t>(head_) + count_) % kQueueDepth;
    Event& e = buf_[tail];
    e.action = action;
    e.flags = flags;
    e.tMs = tMs;
    e.seq = nextSeq_++;
    e.session = session_;
    lastSeq_ = e.seq;
    lastAction_ = action;
    count_++;
    if (action == M4B3::kInputKeyBack) {
      stats_.back++;
    } else if (action == M4B3::kInputKeyReload) {
      stats_.reload++;
    }
    return PushResult::Ok;
  }

  bool pop(Event& out) {
    if (count_ == 0) return false;
    out = buf_[head_];
    head_ = static_cast<uint8_t>((head_ + 1) % kQueueDepth);
    count_--;
    return true;
  }

 private:
  Event buf_[kQueueDepth]{};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  uint8_t lastAction_ = 0;
  uint32_t lastSeq_ = 0;
  uint32_t nextSeq_ = 0;
  uint32_t session_ = 1;
  Stats stats_{};
};

inline M4B3::Status parseInputKey(const uint8_t* header, uint16_t headerLen, uint32_t payloadLen, Event& out) {
  if (payloadLen != 0) return M4B3::Status::Invalid;
  if (headerLen != M4B3::kInputKeyHeaderSize) return M4B3::Status::Invalid;
  if (!header) return M4B3::Status::Invalid;
  out.action = header[0];
  out.flags = header[1];
  out.tMs = M4B3::rd32(header + 4);
  out.seq = M4B3::rd32(header + 8);
  out.session = M4B3::rd32(header + 12);
  if (!validAction(out.action)) return M4B3::Status::Invalid;
  return M4B3::Status::Ok;
}

}  // namespace M4B3Key

#pragma once

// Host-testable M4B3 single-pointer input: panel→logical map + bounded queue.
// No Arduino / TCP / panel presenter deps. FRAME_ACK is independent of this
// path; this module never mutates framebuffer state.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/M4B3Protocol.h"
#include "util/M4PanelMapper.h"

namespace M4B3Input {

constexpr uint16_t kLogicalWidth = M4B3::kWidth;    // 480
constexpr uint16_t kLogicalHeight = M4B3::kHeight;  // 800
constexpr uint16_t kPhysicalWidth = M4PanelMapper::kPhysicalWidth;    // 800
constexpr uint16_t kPhysicalHeight = M4PanelMapper::kPhysicalHeight;  // 480
constexpr size_t kQueueDepth = 8;

// Production evidence (do not invent a second rotate):
//   FT6336U silicon reports portrait rawX=0..479, rawY=0..799.
//   BoardConfig::MURPHY_M4: swapXY=true, flipY=true, raw 0..799 x 0..479
//     so InputManager::getTouchPoint() is already panel-native 800x480.
//   GfxRenderer::tapToLogical Portrait / M4PanelMapper::physicalToLogical:
//     logicalX = 479 - phyY
//     logicalY = phyX
//   BoardConfig comment: default portrait UI maps raw (x,y) back to the
//   same logical (x,y). Silicon raw == Browser-logical after this inverse.

inline bool panelInRange(int physicalX, int physicalY) {
  return physicalX >= 0 && physicalX < static_cast<int>(kPhysicalWidth) && physicalY >= 0 &&
         physicalY < static_cast<int>(kPhysicalHeight);
}

inline bool logicalInRange(int logicalX, int logicalY) {
  return logicalX >= 0 && logicalX < static_cast<int>(kLogicalWidth) && logicalY >= 0 &&
         logicalY < static_cast<int>(kLogicalHeight);
}

inline bool panelToLogical(int physicalX, int physicalY, int* logicalX, int* logicalY) {
  if (!logicalX || !logicalY) return false;
  if (!panelInRange(physicalX, physicalY)) return false;
  M4PanelMapper::physicalToLogical(physicalX, physicalY, logicalX, logicalY);
  return logicalInRange(*logicalX, *logicalY);
}

inline bool validAction(uint8_t action) { return M4B3::validTouchAction(action); }

inline bool isTerminal(uint8_t action) {
  return action == M4B3::kTouchUp || action == M4B3::kTouchCancel;
}

struct Event {
  uint8_t action = 0;
  uint8_t flags = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  uint32_t tMs = 0;
  uint32_t seq = 0;
  uint32_t session = 0;
};

struct Stats {
  uint32_t down = 0;
  uint32_t move = 0;
  uint32_t up = 0;
  uint32_t cancel = 0;
  uint32_t coalesced = 0;
  uint32_t droppedMove = 0;
  uint32_t rejected = 0;
  uint32_t overflow = 0;
  uint32_t sessionResets = 0;
  uint32_t sessionCancels = 0;
  uint32_t implicitCancel = 0;
};

enum class PushResult : uint8_t {
  Ok = 0,
  Coalesced,
  DroppedMove,
  Rejected,
  Overflow,
};

// Single-pointer bounded queue. DOWN/UP/CANCEL are lossless (evict a MOVE
// if the ring is full). MOVE is latest-wins: at most one MOVE is stored and
// a newer MOVE overwrites it. Reconnect/session reset never keeps a stale
// pointer. A DOWN while already active synthesizes CANCEL first.
class Queue {
 public:
  void resetSession() {
    if (active_) {
      stats_.sessionCancels++;
    }
    head_ = 0;
    count_ = 0;
    active_ = false;
    nextSeq_ = 0;
    session_++;
    stats_.sessionResets++;
  }

  uint32_t session() const { return session_; }
  bool active() const { return active_; }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  uint16_t lastX() const { return lastX_; }
  uint16_t lastY() const { return lastY_; }
  uint32_t lastSeq() const { return lastSeq_; }
  const Stats& stats() const { return stats_; }
  Stats& stats() { return stats_; }

  PushResult push(uint8_t action, uint16_t x, uint16_t y, uint32_t tMs, uint8_t flags = 0) {
    if (!validAction(action) || !logicalInRange(x, y)) {
      stats_.rejected++;
      return PushResult::Rejected;
    }

    if (action == M4B3::kTouchDown) {
      if (active_) {
        (void)enqueueLossless(M4B3::kTouchCancel, lastX_, lastY_, tMs, flags);
        stats_.implicitCancel++;
        active_ = false;
      }
      const PushResult st = enqueueLossless(action, x, y, tMs, flags);
      if (st == PushResult::Ok || st == PushResult::Coalesced) {
        active_ = true;
        lastX_ = x;
        lastY_ = y;
      }
      return st;
    }

    if (!active_) {
      stats_.rejected++;
      return PushResult::Rejected;
    }

    if (action == M4B3::kTouchMove) {
      lastX_ = x;
      lastY_ = y;
      return enqueueMove(x, y, tMs, flags);
    }

    const PushResult st = enqueueLossless(action, x, y, tMs, flags);
    if (st == PushResult::Ok || st == PushResult::Coalesced) {
      active_ = false;
      lastX_ = x;
      lastY_ = y;
    }
    return st;
  }

  bool pop(Event& out) {
    if (count_ == 0) return false;
    out = buf_[head_];
    head_ = static_cast<uint8_t>((head_ + 1) % kQueueDepth);
    count_--;
    return true;
  }

 private:
  void note(uint8_t action) {
    switch (action) {
      case M4B3::kTouchDown:
        stats_.down++;
        break;
      case M4B3::kTouchMove:
        stats_.move++;
        break;
      case M4B3::kTouchUp:
        stats_.up++;
        break;
      case M4B3::kTouchCancel:
        stats_.cancel++;
        break;
      default:
        break;
    }
  }

  int findMoveIndex() const {
    for (size_t i = 0; i < count_; ++i) {
      const size_t idx = (static_cast<size_t>(head_) + i) % kQueueDepth;
      if (buf_[idx].action == M4B3::kTouchMove) return static_cast<int>(idx);
    }
    return -1;
  }

  bool evictMove() {
    const int idx = findMoveIndex();
    if (idx < 0) return false;
    size_t dst = static_cast<size_t>(idx);
    while (true) {
      const size_t nxt = (dst + 1) % kQueueDepth;
      const size_t tail = (static_cast<size_t>(head_) + count_ - 1) % kQueueDepth;
      if (dst == tail) break;
      buf_[dst] = buf_[nxt];
      dst = nxt;
    }
    count_--;
    stats_.droppedMove++;
    return true;
  }

  Event make(uint8_t action, uint16_t x, uint16_t y, uint32_t tMs, uint8_t flags) {
    Event e;
    e.action = action;
    e.flags = flags;
    e.x = x;
    e.y = y;
    e.tMs = tMs;
    e.seq = nextSeq_++;
    e.session = session_;
    lastSeq_ = e.seq;
    return e;
  }

  PushResult enqueueMove(uint16_t x, uint16_t y, uint32_t tMs, uint8_t flags) {
    const int idx = findMoveIndex();
    if (idx >= 0) {
      Event& slot = buf_[static_cast<size_t>(idx)];
      slot.x = x;
      slot.y = y;
      slot.tMs = tMs;
      slot.flags = flags;
      slot.seq = nextSeq_++;
      slot.session = session_;
      lastSeq_ = slot.seq;
      stats_.coalesced++;
      stats_.move++;
      return PushResult::Coalesced;
    }
    if (count_ == kQueueDepth) {
      stats_.droppedMove++;
      stats_.coalesced++;
      stats_.move++;
      return PushResult::DroppedMove;
    }
    const size_t tail = (static_cast<size_t>(head_) + count_) % kQueueDepth;
    buf_[tail] = make(M4B3::kTouchMove, x, y, tMs, flags);
    count_++;
    note(M4B3::kTouchMove);
    return PushResult::Ok;
  }

  PushResult enqueueLossless(uint8_t action, uint16_t x, uint16_t y, uint32_t tMs, uint8_t flags) {
    if (count_ == kQueueDepth && !evictMove()) {
      stats_.overflow++;
      return PushResult::Overflow;
    }
    const size_t tail = (static_cast<size_t>(head_) + count_) % kQueueDepth;
    buf_[tail] = make(action, x, y, tMs, flags);
    count_++;
    note(action);
    return PushResult::Ok;
  }

  Event buf_[kQueueDepth]{};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  bool active_ = false;
  uint16_t lastX_ = 0;
  uint16_t lastY_ = 0;
  uint32_t lastSeq_ = 0;
  uint32_t nextSeq_ = 0;
  uint32_t session_ = 1;
  Stats stats_{};
};

inline M4B3::Status parseTouch(const uint8_t* header, uint16_t headerLen, uint32_t payloadLen, Event& out) {
  if (payloadLen != 0) return M4B3::Status::Invalid;
  if (headerLen != M4B3::kTouchHeaderSize) return M4B3::Status::Invalid;
  if (!header) return M4B3::Status::Invalid;
  out.action = header[0];
  out.flags = header[1];
  out.x = M4B3::rd16(header + 4);
  out.y = M4B3::rd16(header + 6);
  out.tMs = M4B3::rd32(header + 8);
  out.seq = M4B3::rd32(header + 12);
  out.session = M4B3::rd32(header + 16);
  if (!validAction(out.action)) return M4B3::Status::Invalid;
  if (!logicalInRange(out.x, out.y)) return M4B3::Status::Invalid;
  return M4B3::Status::Ok;
}

}  // namespace M4B3Input

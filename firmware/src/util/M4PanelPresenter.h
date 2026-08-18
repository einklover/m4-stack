#pragma once

// Host-testable Browser Bridge full-refresh scheduler.
//
// Three concepts stay separate:
//   1. M4B3 accepted logical framebuffer (session / ACK)
//   2. mapped 800x480 panel snapshot (caller-owned, not this object)
//   3. physical refresh scheduling / completion (this object)
//
// Pending depth is exactly one latest-frame-wins slot. Network FRAME_ACK is
// not represented here and must not wait on present completion.

#include <cstdint>

namespace M4PanelPresenter {

constexpr uint32_t kMinIntervalMs = 2000;
constexpr uint32_t kPendingDepth = 1;

enum class Owner : uint8_t { None = 0, Ui = 1, BrowserBridge = 2 };

enum class OfferStatus : uint8_t {
  Scheduled = 0,
  Coalesced = 1,
  DroppedNoOwner = 2,
  DroppedNoBuffer = 3,
};

enum class TakeStatus : uint8_t {
  Ready = 0,
  Idle = 1,
  Busy = 2,
  Interval = 3,
  NoOwner = 4,
};

enum class Error : uint32_t {
  None = 0,
  NoFramebuffer = 1,
  BadSize = 2,
  MapFailed = 3,
  AllocFailed = 4,
  DisplayFailed = 5,
};

struct State {
  Owner owner = Owner::None;
  bool busy = false;
  bool pending = false;
  bool wantRelease = false;
  uint32_t requested = 0;
  uint32_t completed = 0;
  uint32_t coalesced = 0;
  uint32_t dropped = 0;
  uint32_t presentErrors = 0;
  uint32_t mapErrors = 0;
  int32_t pendingFrameId = -1;
  uint32_t pendingCrc = 0;
  int32_t inflightFrameId = -1;
  uint32_t inflightCrc = 0;
  int32_t lastSourceFrameId = -1;
  uint32_t lastSourceCrc = 0;
  uint32_t lastPanelCrc = 0;
  uint32_t lastRequestMs = 0;
  uint32_t lastCompleteMs = 0;
  uint32_t lastError = 0;
  uint32_t minIntervalMs = kMinIntervalMs;
};

class Scheduler {
 public:
  void reset() { st_ = State{}; }

  void setMinIntervalMs(uint32_t ms) { st_.minIntervalMs = ms; }

  bool acquire(Owner who) {
    if (who != Owner::BrowserBridge) return false;
    if (st_.owner == Owner::BrowserBridge) return true;
    st_.owner = Owner::BrowserBridge;
    st_.wantRelease = false;
    return true;
  }

  bool release() {
    if (st_.owner != Owner::BrowserBridge) return false;
    if (st_.busy) {
      st_.wantRelease = true;
      return true;
    }
    finishRelease();
    return true;
  }

  OfferStatus offer(int32_t frameId, uint32_t sourceCrc, uint32_t nowMs) {
    if (st_.owner != Owner::BrowserBridge) {
      st_.dropped++;
      return OfferStatus::DroppedNoOwner;
    }
    const bool hadPending = st_.pending;
    st_.pending = true;
    st_.pendingFrameId = frameId;
    st_.pendingCrc = sourceCrc;
    st_.lastSourceFrameId = frameId;
    st_.lastSourceCrc = sourceCrc;
    st_.lastRequestMs = nowMs;
    st_.requested++;
    if (hadPending) {
      st_.coalesced++;
      return OfferStatus::Coalesced;
    }
    return OfferStatus::Scheduled;
  }

  TakeStatus take(uint32_t nowMs, int32_t* frameId = nullptr, uint32_t* sourceCrc = nullptr) {
    if (frameId) *frameId = -1;
    if (sourceCrc) *sourceCrc = 0;
    if (st_.owner != Owner::BrowserBridge) return TakeStatus::NoOwner;
    if (st_.busy) return TakeStatus::Busy;
    if (!st_.pending) return TakeStatus::Idle;
    if (st_.lastCompleteMs != 0 && (nowMs - st_.lastCompleteMs) < st_.minIntervalMs) {
      return TakeStatus::Interval;
    }
    st_.busy = true;
    st_.inflightFrameId = st_.pendingFrameId;
    st_.inflightCrc = st_.pendingCrc;
    st_.pending = false;
    if (frameId) *frameId = st_.inflightFrameId;
    if (sourceCrc) *sourceCrc = st_.inflightCrc;
    return TakeStatus::Ready;
  }

  void complete(bool ok, uint32_t panelCrc, uint32_t nowMs, uint32_t error = 0) {
    if (!st_.busy) return;
    st_.busy = false;
    if (ok) {
      st_.completed++;
      st_.lastCompleteMs = nowMs;
      st_.lastPanelCrc = panelCrc;
      st_.lastError = 0;
    } else {
      st_.presentErrors++;
      st_.lastError = error ? error : static_cast<uint32_t>(Error::DisplayFailed);
      if (!st_.pending) {
        st_.pending = true;
        st_.pendingFrameId = st_.inflightFrameId;
        st_.pendingCrc = st_.inflightCrc;
      }
    }
    st_.inflightFrameId = -1;
    st_.inflightCrc = 0;
    if (st_.wantRelease && !st_.busy) finishRelease();
  }

  void noteMapError() { st_.mapErrors++; }

  void noteDroppedNoBuffer() { st_.dropped++; }

  const State& state() const { return st_; }

  bool owns(Owner who) const { return st_.owner == who; }

  bool browserOwns() const { return st_.owner == Owner::BrowserBridge; }

  static constexpr uint32_t pendingDepth() { return kPendingDepth; }

 private:
  void finishRelease() {
    st_.owner = Owner::Ui;
    st_.pending = false;
    st_.wantRelease = false;
    st_.pendingFrameId = -1;
    st_.pendingCrc = 0;
    st_.inflightFrameId = -1;
    st_.inflightCrc = 0;
  }

  State st_{};
};

}  // namespace M4PanelPresenter

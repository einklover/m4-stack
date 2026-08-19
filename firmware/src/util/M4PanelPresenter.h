#pragma once

// Host-testable Browser Bridge panel scheduler.
//
// Four concepts stay separate:
//   1. M4B3 accepted logical framebuffer (session / ACK)
//   2. newest mapped 800x480 snapshot (caller-owned)
//   3. last successfully physically presented 800x480 snapshot (caller-owned)
//   4. physical refresh scheduling / completion (this object)
//
// Pending depth is exactly one latest-frame-wins slot. Network FRAME_ACK is
// not represented here and must not wait on present completion. Dirty/policy
// is computed by the caller against #3 at take time, including coalesced frames.

#include <cstdint>
#include <cstring>

#include "util/M4PanelDirty.h"

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
  bool baselineTrusted = false;
  bool everPresented = false;
  bool injectNextFail = false;
  uint32_t requested = 0;
  uint32_t completed = 0;
  uint32_t coalesced = 0;
  uint32_t dropped = 0;
  uint32_t presentErrors = 0;
  uint32_t mapErrors = 0;
  uint32_t fullReq = 0;
  uint32_t fullOk = 0;
  uint32_t fullErr = 0;
  uint32_t partialReq = 0;
  uint32_t partialOk = 0;
  uint32_t partialErr = 0;
  uint32_t hygieneReq = 0;
  uint32_t hygieneOk = 0;
  uint32_t hygieneErr = 0;
  uint32_t noChange = 0;
  uint32_t partialsSinceFull = 0;
  uint32_t cumulativePartialPixels = 0;
  uint32_t uniqueCoveragePixels = 0;
  uint32_t coverageBits[M4PanelDirty::kCoverageWords] = {};
  uint32_t lastDirtyPixels = 0;
  uint32_t lastDirtyArea = 0;
  uint16_t lastRectCount = 0;
  uint32_t lastPolicyReason = 0;
  uint32_t lastFullMs = 0;
  uint32_t lastPartialMs = 0;
  uint32_t lastHygieneMs = 0;
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
  uint32_t baselineEpoch = 0;
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
    // Physical baseline is untrusted after a new acquire. First present is a
    // full-panel FAST (never OTP FULL).
    st_.baselineTrusted = false;
    st_.everPresented = false;
    resetHygieneLocked();
    st_.baselineEpoch++;
    return true;
  }

  // lastPresented may stay in RAM, but it is no longer known to match glass.
  // Next non-empty take decides ForcedFullRecovery (everPresented stays true).
  void invalidatePhysicalBaseline() {
    st_.baselineTrusted = false;
    st_.baselineEpoch++;
  }

  // Panel init / boot / hardware re-init: previous pixels are unknowable.
  // Next take decides FirstBaseline even if lastPresented still matches pending.
  void notePanelReinit() {
    st_.baselineTrusted = false;
    st_.everPresented = false;
    resetHygieneLocked();
    st_.baselineEpoch++;
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

  M4PanelDirty::Decision decide(const M4PanelDirty::Plan& plan) const {
    uint32_t bits[M4PanelDirty::kCoverageWords];
    std::memcpy(bits, st_.coverageBits, sizeof(bits));
    M4PanelDirty::markPlanCoverage(bits, plan);
    return M4PanelDirty::decide(st_.baselineTrusted, st_.everPresented, plan, st_.partialsSinceFull,
                                st_.cumulativePartialPixels, M4PanelDirty::coveragePixels(bits));
  }

  void notePolicy(M4PanelDirty::Mode mode, M4PanelDirty::Reason reason, uint32_t dirtyPixels, uint32_t windowArea,
                  uint16_t rectCount, const M4PanelDirty::Plan* plan = nullptr) {
    st_.lastPolicyReason = static_cast<uint32_t>(reason);
    st_.lastDirtyPixels = dirtyPixels;
    st_.lastDirtyArea = windowArea;
    st_.lastRectCount = rectCount;
    if (mode == M4PanelDirty::Mode::Full) st_.fullReq++;
    if (mode == M4PanelDirty::Mode::Partial) st_.partialReq++;
    if (mode == M4PanelDirty::Mode::Hygiene) st_.hygieneReq++;
    if (mode == M4PanelDirty::Mode::Skip) st_.noChange++;
    if (plan && mode == M4PanelDirty::Mode::Partial) {
      M4PanelDirty::markPlanCoverage(st_.coverageBits, *plan);
      st_.uniqueCoveragePixels = M4PanelDirty::coveragePixels(st_.coverageBits);
    }
  }

  void injectNextFailure() { st_.injectNextFail = true; }

  bool consumeInjectedFailure() {
    const bool v = st_.injectNextFail;
    st_.injectNextFail = false;
    return v;
  }

  void complete(bool ok, uint32_t panelCrc, uint32_t nowMs, uint32_t error = 0,
                M4PanelDirty::Mode mode = M4PanelDirty::Mode::Full, uint32_t elapsedMs = 0,
                uint32_t dirtyPixels = 0, uint32_t windowArea = 0, uint16_t rectCount = 0,
                M4PanelDirty::Reason reason = M4PanelDirty::Reason::None) {
    if (!st_.busy) return;
    st_.busy = false;
    if (reason != M4PanelDirty::Reason::None) st_.lastPolicyReason = static_cast<uint32_t>(reason);
    st_.lastDirtyPixels = dirtyPixels;
    st_.lastDirtyArea = windowArea;
    st_.lastRectCount = rectCount;
    if (ok) {
      if (mode == M4PanelDirty::Mode::Skip) {
        st_.lastError = 0;
      } else {
        st_.completed++;
        st_.lastCompleteMs = nowMs;
        st_.lastPanelCrc = panelCrc;
        st_.lastError = 0;
        if (mode == M4PanelDirty::Mode::Partial) {
          st_.partialOk++;
          st_.lastPartialMs = elapsedMs;
          st_.partialsSinceFull = M4PanelDirty::satAdd(st_.partialsSinceFull, 1);
          st_.cumulativePartialPixels = M4PanelDirty::satAdd(st_.cumulativePartialPixels, dirtyPixels);
          st_.baselineTrusted = true;
          st_.everPresented = true;
        } else if (mode == M4PanelDirty::Mode::Hygiene) {
          st_.hygieneOk++;
          st_.lastHygieneMs = elapsedMs;
          resetHygieneLocked();
          st_.baselineTrusted = true;
          st_.everPresented = true;
        } else {
          st_.fullOk++;
          st_.lastFullMs = elapsedMs;
          resetHygieneLocked();
          st_.baselineTrusted = true;
          st_.everPresented = true;
        }
      }
    } else {
      st_.presentErrors++;
      st_.lastError = error ? error : static_cast<uint32_t>(Error::DisplayFailed);
      if (mode == M4PanelDirty::Mode::Partial) {
        st_.partialErr++;
        st_.lastPartialMs = elapsedMs;
      } else if (mode == M4PanelDirty::Mode::Hygiene) {
        st_.hygieneErr++;
        st_.lastHygieneMs = elapsedMs;
      } else {
        st_.fullErr++;
        st_.lastFullMs = elapsedMs;
      }
      // Do not advance lastPresented (caller). Baseline is untrusted; retry FULL.
      st_.baselineTrusted = false;
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
    st_.baselineTrusted = false;
    st_.everPresented = false;
    resetHygieneLocked();
    st_.baselineEpoch++;
  }

  void resetHygieneLocked() {
    st_.partialsSinceFull = 0;
    st_.cumulativePartialPixels = 0;
    st_.uniqueCoveragePixels = 0;
    M4PanelDirty::clearCoverage(st_.coverageBits);
  }

  State st_{};
};

}  // namespace M4PanelPresenter

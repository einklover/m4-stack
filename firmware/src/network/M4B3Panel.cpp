#if defined(CROSSPOINT_MURPHY_M4)

#include "network/M4B3Panel.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "apps/providers/M4Psram.h"
#include "util/M4B3Protocol.h"
#include "util/M4DisplayOwner.h"
#include "util/M4PanelDirty.h"
#include "util/M4PanelMapper.h"
#include "util/M4PanelPresenter.h"

bool m4BrowserBridgeOwnsDisplay() { return M4B3Panel::browserOwnsDisplay(); }

void m4BrowserBridgeInvalidatePhysicalBaseline() { M4B3Panel::invalidatePhysicalBaseline(); }

void m4BrowserBridgeNotePanelReinit() { M4B3Panel::notePanelReinit(); }

namespace M4B3Panel {
namespace {

constexpr size_t kPanelBytes = M4PanelMapper::kPhysicalSize;
static_assert(kPanelBytes == 48000u, "panel snapshot is 48,000 B");

struct Runtime {
  M4PanelPresenter::Scheduler sched;
  std::mutex mu;
  uint8_t* scratch = nullptr;
  uint8_t* pending = nullptr;
  uint8_t* presented = nullptr;
  std::atomic<bool> ready{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> owns{false};
  uint8_t lastCorner[4] = {};
  uint16_t lastWin[4][4] = {};
};

Runtime gRt;

void publishOwner() { gRt.owns.store(gRt.sched.browserOwns(), std::memory_order_release); }

void copyCorners(const uint8_t* fb) {
  if (!fb) return;
  gRt.lastCorner[0] = fb[0];
  gRt.lastCorner[1] = fb[99];
  gRt.lastCorner[2] = fb[47900];
  gRt.lastCorner[3] = fb[47999];
}

void storeWindows(const M4PanelDirty::Plan& plan) {
  std::memset(gRt.lastWin, 0, sizeof(gRt.lastWin));
  const uint16_t n = plan.windowCount > 4 ? 4 : plan.windowCount;
  for (uint16_t i = 0; i < n; ++i) {
    gRt.lastWin[i][0] = plan.windows[i].x;
    gRt.lastWin[i][1] = plan.windows[i].y;
    gRt.lastWin[i][2] = plan.windows[i].w;
    gRt.lastWin[i][3] = plan.windows[i].h;
  }
}

void logState(const char* why, uint32_t nowMs) {
  Snapshot s;
  snapshot(s, nowMs);
  Serial.printf(
      "[%lu] [M4B3-PANEL] %s owner=%u busy=%d pend=%d trust=%d ever=%d epoch=%u req=%u ok=%u coal=%u drop=%u "
      "full=%u/%u/%u part=%u/%u/%u nochg=%u n=%u cum=%u dirty=%u area=%u rects=%u reason=%s "
      "full_ms=%u part_ms=%u src=%ld crc=0x%08x panel=0x%08x err=%u age=%u "
      "corners=%02x/%02x/%02x/%02x win=%u,%u %ux%u / %u,%u %ux%u heap=%u psram=%u\n",
      static_cast<unsigned long>(nowMs), why, static_cast<unsigned>(s.owner), s.busy ? 1 : 0,
      s.pending ? 1 : 0, s.baselineTrusted ? 1 : 0, s.everPresented ? 1 : 0, s.baselineEpoch, s.requested,
      s.completed, s.coalesced, s.dropped,
      s.fullReq, s.fullOk, s.fullErr, s.partialReq, s.partialOk, s.partialErr, s.noChange,
      s.partialsSinceFull, s.cumulativePartialPixels, s.lastDirtyPixels, s.lastDirtyArea,
      static_cast<unsigned>(s.lastRectCount),
      M4PanelDirty::reasonName(static_cast<M4PanelDirty::Reason>(s.lastPolicyReason)), s.lastFullMs,
      s.lastPartialMs, static_cast<long>(s.sourceFrameId), static_cast<unsigned>(s.sourceCrc),
      static_cast<unsigned>(s.panelCrc), s.lastError, s.ageMs, s.corner[0], s.corner[1], s.corner[2],
      s.corner[3], s.lastWin[0][0], s.lastWin[0][1], s.lastWin[0][2], s.lastWin[0][3], s.lastWin[1][0],
      s.lastWin[1][1], s.lastWin[1][2], s.lastWin[1][3], static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getFreePsram()));
}

void finishFailed(uint32_t nowMs, uint32_t err, M4PanelDirty::Mode mode, M4PanelDirty::Reason reason,
                  uint32_t dirtyPixels, uint32_t windowArea, uint16_t rects, uint32_t elapsedMs) {
  gRt.sched.complete(false, 0, nowMs, err, mode, elapsedMs, dirtyPixels, windowArea, rects, reason);
}

}  // namespace

bool begin() {
  if (gRt.ready.load(std::memory_order_relaxed)) return true;
  gRt.scratch = static_cast<uint8_t*>(M4Psram::mallocPrefer(kPanelBytes));
  gRt.pending = static_cast<uint8_t*>(M4Psram::mallocPrefer(kPanelBytes));
  gRt.presented = static_cast<uint8_t*>(M4Psram::mallocPrefer(kPanelBytes));
  if (!gRt.scratch || !gRt.pending || !gRt.presented) {
    Serial.printf("[%lu] [M4B3-PANEL] PSRAM alloc failed scratch=%p pending=%p presented=%p\n", millis(),
                  static_cast<void*>(gRt.scratch), static_cast<void*>(gRt.pending),
                  static_cast<void*>(gRt.presented));
    M4Psram::freePrefer(gRt.scratch);
    M4Psram::freePrefer(gRt.pending);
    M4Psram::freePrefer(gRt.presented);
    gRt.scratch = nullptr;
    gRt.pending = nullptr;
    gRt.presented = nullptr;
    return false;
  }
  std::memset(gRt.scratch, 0xFF, kPanelBytes);
  std::memset(gRt.pending, 0xFF, kPanelBytes);
  std::memset(gRt.presented, 0xFF, kPanelBytes);
  gRt.sched.reset();
  gRt.ready.store(true, std::memory_order_release);
  Serial.printf("[%lu] [M4B3-PANEL] ready snapshot=%u minInterval=%u maxPartialPct=28 maxWin=%u "
                "maxPartialN=%u\n",
                millis(), static_cast<unsigned>(kPanelBytes),
                static_cast<unsigned>(M4PanelPresenter::kMinIntervalMs),
                static_cast<unsigned>(M4PanelDirty::kMaxWindows),
                static_cast<unsigned>(M4PanelDirty::kMaxPartialsSinceFull));
  return true;
}

void offerAccepted(const uint8_t* logical, size_t logicalLen, int32_t frameId, uint32_t sourceCrc) {
  if (!gRt.ready.load(std::memory_order_acquire) || !gRt.scratch || !gRt.pending || !gRt.presented) {
    std::lock_guard<std::mutex> lock(gRt.mu);
    gRt.sched.noteDroppedNoBuffer();
    return;
  }
  if (!logical || logicalLen != M4PanelMapper::kLogicalSize) {
    std::lock_guard<std::mutex> lock(gRt.mu);
    gRt.sched.noteMapError();
    return;
  }

  gRt.connected.store(true, std::memory_order_relaxed);
  bool acquired = false;
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    if (!gRt.sched.browserOwns()) {
      gRt.sched.acquire(M4PanelPresenter::Owner::BrowserBridge);
      publishOwner();
      acquired = true;
    }
  }
  if (acquired) {
    Serial.printf("[%lu] [M4B3-PANEL] acquire owner=browser src=%ld crc=0x%08x\n", millis(),
                  static_cast<long>(frameId), static_cast<unsigned>(sourceCrc));
  }

  const M4PanelMapper::Status st =
      M4PanelMapper::mapLogicalToPhysical(logical, logicalLen, gRt.scratch, kPanelBytes, nullptr);
  if (st != M4PanelMapper::Status::Ok) {
    std::lock_guard<std::mutex> lock(gRt.mu);
    gRt.sched.noteMapError();
    Serial.printf("[%lu] [M4B3-PANEL] map failed status=%u src=%ld\n", millis(),
                  static_cast<unsigned>(st), static_cast<long>(frameId));
    return;
  }

  M4PanelPresenter::OfferStatus os;
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    std::memcpy(gRt.pending, gRt.scratch, kPanelBytes);
    os = gRt.sched.offer(frameId, sourceCrc, millis());
  }
  if (os == M4PanelPresenter::OfferStatus::Coalesced) {
    Serial.printf("[%lu] [M4B3-PANEL] coalesce src=%ld crc=0x%08x\n", millis(),
                  static_cast<long>(frameId), static_cast<unsigned>(sourceCrc));
  }
}

void noteDisconnect() {
  const bool was = gRt.connected.exchange(false, std::memory_order_relaxed);
  uint32_t epoch = 0;
  uint8_t owner = 0;
  bool busy = false;
  bool owned = false;
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    owned = gRt.sched.browserOwns();
    owner = static_cast<uint8_t>(gRt.sched.state().owner);
    busy = gRt.sched.state().busy;
    // TCP drop does not itself paint the glass, but the physical baseline is
    // no longer guaranteed: Home/UI may take the panel before the next tick
    // release, or a fast reconnect may keep owner and skip FirstBaseline.
    if (was || owned || gRt.sched.state().baselineTrusted) {
      gRt.sched.invalidatePhysicalBaseline();
    }
    epoch = gRt.sched.state().baselineEpoch;
  }
  if (was || owned) {
    Serial.printf("[%lu] [M4B3-PANEL] disconnect owner=%u busy=%d epoch=%u\n", millis(),
                  static_cast<unsigned>(owner), busy ? 1 : 0, static_cast<unsigned>(epoch));
  }
}

void invalidatePhysicalBaseline() {
  if (!gRt.ready.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(gRt.mu);
  gRt.sched.invalidatePhysicalBaseline();
}

void notePanelReinit() {
  if (!gRt.ready.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(gRt.mu);
  gRt.sched.notePanelReinit();
}

bool maybeReleaseLocked() {
  if (gRt.connected.load(std::memory_order_relaxed)) return false;
  if (!gRt.sched.browserOwns() || gRt.sched.state().busy) return false;
  gRt.sched.release();
  publishOwner();
  return true;
}

void tick(HalDisplay& display, uint32_t nowMs) {
  if (!gRt.ready.load(std::memory_order_acquire)) return;

  int32_t frameId = -1;
  uint32_t sourceCrc = 0;
  bool released = false;
  M4PanelPresenter::TakeStatus ts;
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    released = maybeReleaseLocked();
    ts = released ? M4PanelPresenter::TakeStatus::NoOwner : gRt.sched.take(nowMs, &frameId, &sourceCrc);
  }
  if (released) logState("release", nowMs);
  if (ts != M4PanelPresenter::TakeStatus::Ready) return;

  uint8_t* fb = display.getFrameBuffer();
  if (!fb || HalDisplay::BUFFER_SIZE != kPanelBytes || !gRt.presented) {
    {
      std::lock_guard<std::mutex> lock(gRt.mu);
      finishFailed(nowMs, static_cast<uint32_t>(M4PanelPresenter::Error::NoFramebuffer),
                   M4PanelDirty::Mode::Full, M4PanelDirty::Reason::UntrustedBaseline, 0, 0, 0, 0);
    }
    logState("no-fb", nowMs);
    return;
  }

  M4PanelDirty::Plan plan{};
  M4PanelDirty::Decision dec{};
  bool injectFail = false;
  bool planOk = true;
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    if (!M4PanelDirty::plan(gRt.presented, gRt.pending, kPanelBytes, plan)) {
      finishFailed(nowMs, static_cast<uint32_t>(M4PanelPresenter::Error::BadSize), M4PanelDirty::Mode::Full,
                   M4PanelDirty::Reason::UntrustedBaseline, 0, 0, 0, 0);
      planOk = false;
    } else {
      dec = gRt.sched.decide(plan);
      storeWindows(plan);
      gRt.sched.notePolicy(dec.mode, dec.reason, plan.changedPixels, plan.windowArea, plan.windowCount);
      injectFail = gRt.sched.consumeInjectedFailure();
      std::memcpy(fb, gRt.pending, kPanelBytes);
    }
  }
  if (!planOk) {
    logState("plan-fail", nowMs);
    return;
  }

  if (dec.mode == M4PanelDirty::Mode::Skip) {
    const uint32_t panelCrc = M4B3::crc32(gRt.presented, kPanelBytes);
    copyCorners(gRt.presented);
    {
      std::lock_guard<std::mutex> lock(gRt.mu);
      gRt.sched.complete(true, panelCrc, nowMs, 0, M4PanelDirty::Mode::Skip, 0, 0, 0, 0, dec.reason);
      released = maybeReleaseLocked();
    }
    logState("no-change", nowMs);
    if (released) logState("release", millis());
    return;
  }

  if (injectFail) {
    {
      std::lock_guard<std::mutex> lock(gRt.mu);
      finishFailed(nowMs, static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed), dec.mode,
                   dec.reason, plan.changedPixels, plan.windowArea, plan.windowCount, 0);
    }
    logState("inject-fail", nowMs);
    return;
  }

  const uint32_t panelCrc = M4B3::crc32(fb, kPanelBytes);
  copyCorners(fb);
  Serial.printf("[%lu] [M4B3-PANEL] present-start mode=%s reason=%s src=%ld crc=0x%08x panel=0x%08x "
                "dirty=%u area=%u rects=%u\n",
                static_cast<unsigned long>(nowMs),
                dec.mode == M4PanelDirty::Mode::Partial ? "partial" : "full",
                M4PanelDirty::reasonName(dec.reason), static_cast<long>(frameId),
                static_cast<unsigned>(sourceCrc), static_cast<unsigned>(panelCrc), plan.changedPixels,
                plan.windowArea, static_cast<unsigned>(plan.windowCount));

  const uint32_t t0 = millis();
  bool ok = true;
  if (dec.mode == M4PanelDirty::Mode::Partial) {
    const uint16_t n = plan.windowCount > M4PanelDirty::kMaxWindows ? M4PanelDirty::kMaxWindows
                                                                   : plan.windowCount;
    for (uint16_t i = 0; i < n; ++i) {
      const M4PanelDirty::Rect& r = plan.windows[i];
      if (!display.displayWindow(r.x, r.y, r.w, r.h, false)) {
        ok = false;
        Serial.printf("[%lu] [M4B3-PANEL] window-fail %u,%u %ux%u\n", millis(), r.x, r.y, r.w, r.h);
        break;
      }
    }
  } else {
    display.displayBuffer(HalDisplay::FULL_REFRESH);
  }
  const uint32_t doneMs = millis();
  const uint32_t elapsed = doneMs - t0;

  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    if (ok) {
      std::memcpy(gRt.presented, gRt.pending, kPanelBytes);
      gRt.sched.complete(true, panelCrc, doneMs, 0, dec.mode, elapsed, plan.changedPixels, plan.windowArea,
                         plan.windowCount, dec.reason);
    } else {
      finishFailed(doneMs, static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed), dec.mode,
                   dec.reason, plan.changedPixels, plan.windowArea, plan.windowCount, elapsed);
    }
    released = maybeReleaseLocked();
  }
  logState(ok ? "present-ok" : "present-err", doneMs);
  if (released) logState("release", millis());
}

bool browserOwnsDisplay() { return gRt.owns.load(std::memory_order_acquire); }

void injectNextFailure() {
  std::lock_guard<std::mutex> lock(gRt.mu);
  gRt.sched.injectNextFailure();
}

void snapshot(Snapshot& out, uint32_t nowMs) {
  M4PanelPresenter::State st;
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    st = gRt.sched.state();
    for (int i = 0; i < 4; ++i) {
      out.lastWin[i][0] = gRt.lastWin[i][0];
      out.lastWin[i][1] = gRt.lastWin[i][1];
      out.lastWin[i][2] = gRt.lastWin[i][2];
      out.lastWin[i][3] = gRt.lastWin[i][3];
    }
  }
  out.owner = static_cast<uint8_t>(st.owner);
  out.busy = st.busy;
  out.pending = st.pending;
  out.baselineTrusted = st.baselineTrusted;
  out.everPresented = st.everPresented;
  out.baselineEpoch = st.baselineEpoch;
  out.requested = st.requested;
  out.completed = st.completed;
  out.coalesced = st.coalesced;
  out.dropped = st.dropped;
  out.presentErrors = st.presentErrors;
  out.mapErrors = st.mapErrors;
  out.fullReq = st.fullReq;
  out.fullOk = st.fullOk;
  out.fullErr = st.fullErr;
  out.partialReq = st.partialReq;
  out.partialOk = st.partialOk;
  out.partialErr = st.partialErr;
  out.noChange = st.noChange;
  out.partialsSinceFull = st.partialsSinceFull;
  out.cumulativePartialPixels = st.cumulativePartialPixels;
  out.lastDirtyPixels = st.lastDirtyPixels;
  out.lastDirtyArea = st.lastDirtyArea;
  out.lastRectCount = st.lastRectCount;
  out.lastPolicyReason = st.lastPolicyReason;
  out.lastFullMs = st.lastFullMs;
  out.lastPartialMs = st.lastPartialMs;
  out.sourceFrameId = st.lastSourceFrameId;
  out.sourceCrc = st.lastSourceCrc;
  out.panelCrc = st.lastPanelCrc;
  out.lastError = st.lastError;
  out.lastRequestMs = st.lastRequestMs;
  out.lastCompleteMs = st.lastCompleteMs;
  if (nowMs == 0) nowMs = millis();
  out.ageMs = (st.lastCompleteMs == 0 || nowMs < st.lastCompleteMs) ? 0 : (nowMs - st.lastCompleteMs);
  out.corner[0] = gRt.lastCorner[0];
  out.corner[1] = gRt.lastCorner[1];
  out.corner[2] = gRt.lastCorner[2];
  out.corner[3] = gRt.lastCorner[3];
}

}  // namespace M4B3Panel

#endif

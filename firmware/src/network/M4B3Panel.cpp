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
#include "util/M4PanelMapper.h"
#include "util/M4PanelPresenter.h"

bool m4BrowserBridgeOwnsDisplay() { return M4B3Panel::browserOwnsDisplay(); }

namespace M4B3Panel {
namespace {

constexpr size_t kPanelBytes = M4PanelMapper::kPhysicalSize;
static_assert(kPanelBytes == 48000u, "panel snapshot is 48,000 B");

struct Runtime {
  M4PanelPresenter::Scheduler sched;
  std::mutex mu;
  uint8_t* scratch = nullptr;
  uint8_t* pending = nullptr;
  std::atomic<bool> ready{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> owns{false};
  uint8_t lastCorner[4] = {};
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

void logState(const char* why, uint32_t nowMs) {
  Snapshot s;
  snapshot(s, nowMs);
  Serial.printf(
      "[%lu] [M4B3-PANEL] %s owner=%u busy=%d pend=%d req=%u ok=%u coal=%u drop=%u "
      "mapErr=%u presErr=%u src=%ld crc=0x%08x panel=0x%08x err=%u age=%u "
      "corners=%02x/%02x/%02x/%02x heap=%u psram=%u\n",
      static_cast<unsigned long>(nowMs), why, static_cast<unsigned>(s.owner), s.busy ? 1 : 0,
      s.pending ? 1 : 0, s.requested, s.completed, s.coalesced, s.dropped, s.mapErrors,
      s.presentErrors, static_cast<long>(s.sourceFrameId), static_cast<unsigned>(s.sourceCrc),
      static_cast<unsigned>(s.panelCrc), s.lastError, s.ageMs, s.corner[0], s.corner[1], s.corner[2],
      s.corner[3], static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getFreePsram()));
}

}  // namespace

bool begin() {
  if (gRt.ready.load(std::memory_order_relaxed)) return true;
  gRt.scratch = static_cast<uint8_t*>(M4Psram::mallocPrefer(kPanelBytes));
  gRt.pending = static_cast<uint8_t*>(M4Psram::mallocPrefer(kPanelBytes));
  if (!gRt.scratch || !gRt.pending) {
    Serial.printf("[%lu] [M4B3-PANEL] PSRAM alloc failed scratch=%p pending=%p\n", millis(),
                  static_cast<void*>(gRt.scratch), static_cast<void*>(gRt.pending));
    M4Psram::freePrefer(gRt.scratch);
    M4Psram::freePrefer(gRt.pending);
    gRt.scratch = nullptr;
    gRt.pending = nullptr;
    return false;
  }
  std::memset(gRt.scratch, 0xFF, kPanelBytes);
  std::memset(gRt.pending, 0xFF, kPanelBytes);
  gRt.sched.reset();
  gRt.ready.store(true, std::memory_order_release);
  Serial.printf("[%lu] [M4B3-PANEL] ready snapshot=%u minInterval=%u\n", millis(),
                static_cast<unsigned>(kPanelBytes),
                static_cast<unsigned>(M4PanelPresenter::kMinIntervalMs));
  return true;
}

void offerAccepted(const uint8_t* logical, size_t logicalLen, int32_t frameId, uint32_t sourceCrc) {
  if (!gRt.ready.load(std::memory_order_acquire) || !gRt.scratch || !gRt.pending) {
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
  if (was || gRt.sched.browserOwns()) {
    Serial.printf("[%lu] [M4B3-PANEL] disconnect owner=%u busy=%d\n", millis(),
                  static_cast<unsigned>(gRt.sched.state().owner), gRt.sched.state().busy ? 1 : 0);
  }
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
  if (!fb || HalDisplay::BUFFER_SIZE != kPanelBytes) {
    {
      std::lock_guard<std::mutex> lock(gRt.mu);
      gRt.sched.complete(false, 0, nowMs, static_cast<uint32_t>(M4PanelPresenter::Error::NoFramebuffer));
    }
    logState("no-fb", nowMs);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    std::memcpy(fb, gRt.pending, kPanelBytes);
  }
  const uint32_t panelCrc = M4B3::crc32(fb, kPanelBytes);
  copyCorners(fb);

  Serial.printf("[%lu] [M4B3-PANEL] present-start src=%ld crc=0x%08x panel=0x%08x\n",
                static_cast<unsigned long>(nowMs), static_cast<long>(frameId),
                static_cast<unsigned>(sourceCrc), static_cast<unsigned>(panelCrc));
  display.displayBuffer(HalDisplay::FULL_REFRESH);
  const uint32_t doneMs = millis();
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    gRt.sched.complete(true, panelCrc, doneMs);
    released = maybeReleaseLocked();
  }
  logState("present-ok", doneMs);
  if (released) logState("release", millis());
}

bool browserOwnsDisplay() { return gRt.owns.load(std::memory_order_acquire); }

void snapshot(Snapshot& out, uint32_t nowMs) {
  M4PanelPresenter::State st;
  {
    std::lock_guard<std::mutex> lock(gRt.mu);
    st = gRt.sched.state();
  }
  out.owner = static_cast<uint8_t>(st.owner);
  out.busy = st.busy;
  out.pending = st.pending;
  out.requested = st.requested;
  out.completed = st.completed;
  out.coalesced = st.coalesced;
  out.dropped = st.dropped;
  out.presentErrors = st.presentErrors;
  out.mapErrors = st.mapErrors;
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

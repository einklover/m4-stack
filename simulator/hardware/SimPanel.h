// SimPanel: SSD1677 e-ink model with THREE distinct frame states and frame
// provenance:
//   renderFB  — what the renderer just drew (may be stale by the time BUSY ends)
//   pending   — frame submitted to the panel, currently refreshing
//   physical  — what the user actually sees (updated only when BUSY completes)
// The panel knows WHICH page (provenance) every frame carries. If firmware
// records lastPhysicalBodyPage_ = live currentPage instead of the pending
// frame's page, the divergence assertion catches it.
#pragma once

#include <cstdint>
#include <functional>

#include "core/SimKernel.h"
#include "platform/DisplayPort.h"

namespace m4sim {

// Backward-compatible names for existing scenarios. New shared code should
// prefer the m4platform names directly.
using RefreshMode = m4platform::RefreshMode;
using SimFrameTag = m4platform::FrameTag;

struct SimFrame {
  SimFrameTag tag;
  bool valid = false;
};

// EPD timing profile (ms). These get calibrated from real-device logs later;
// the numbers mirror the firmware's observed FAST/HALF/FULL durations.
struct EpdProfile {
  uint32_t fastMs = 420;
  uint32_t halfMs = 850;
  uint32_t fullMs = 1800;
  uint32_t animStepMs = 80;
  uint32_t modeMs(RefreshMode m) const {
    switch (m) {
      case RefreshMode::FAST_REFRESH: return fastMs;
      case RefreshMode::HALF_REFRESH: return halfMs;
      case RefreshMode::FULL_REFRESH: return fullMs;
      case RefreshMode::UI_FAST_REFRESH: return fastMs;
    }
    return fastMs;
  }
};

class SimPanel final : public m4platform::DisplayPort {
public:
  SimPanel(SimScheduler* sched, SimTrace* trace, EpdProfile prof = {})
      : sched_(sched), trace_(trace), profile_(prof) {}

  const EpdProfile& profile() const { return profile_; }
  bool busy() const override { return busy_; }

  const SimFrame& renderFB() const { return renderFB_; }
  const SimFrame& pending() const { return pending_; }
  const SimFrame& physical() const { return physical_; }

  // Renderer finished a frame. Records provenance; does NOT touch the panel.
  void render(const SimFrameTag& tag) override {
    renderFB_.tag = tag;
    renderFB_.valid = true;
    char buf[96];
    snprintf(buf, sizeof(buf), "chapter=%d page=%d gen=%u hash=%llx",
             tag.chapter, tag.page, tag.generation, (unsigned long long)tag.frameHash);
    trace_->emit(SimEventType::FRAME_RENDERED, buf, sched_->now());
  }

  // Submit the renderFB to the SSD1677. If a refresh is already in flight,
  // reject (single pending frame — matches SSD1677). Returns true if accepted.
  bool submit(RefreshMode mode, std::function<void()> onCommitted = nullptr) override {
    if (busy_ || !renderFB_.valid) return false;
    pending_ = renderFB_;
    pending_.valid = true;
    renderFB_.valid = false;
    busy_ = true;
    mode_ = mode;
    uint32_t dur = profile_.modeMs(mode);
    char buf[96];
    snprintf(buf, sizeof(buf), "page=%d gen=%u mode=%s duration=%ums",
             pending_.tag.page, pending_.tag.generation, modeName(mode), dur);
    trace_->emit(SimEventType::EPD_SUBMITTED, buf, sched_->now());
    trace_->emit(SimEventType::EPD_BUSY, buf, sched_->now());
    uint32_t t0 = sched_->now();
    uint32_t token = ++refreshToken_;  // invalidates any aborted refresh
    sched_->scheduleAt(t0 + dur, [this, onCommitted, t0, token]() {
      if (token != refreshToken_) return;  // refresh was aborted: no commit, no callback
      commit(t0);
      if (onCommitted) onCommitted();
    });
    return true;
  }

  // BUSY finished: pending becomes physical. This is the ONLY place the
  // physical panel state changes.
  void commit(uint32_t submittedAt) {
    if (!pending_.valid) return;
    physical_ = pending_;
    busy_ = false;
    char buf[96];
    snprintf(buf, sizeof(buf), "page=%d gen=%u hash=%llx submitted_t=%u committed_t=%u",
             physical_.tag.page, physical_.tag.generation,
             (unsigned long long)physical_.tag.frameHash, submittedAt, sched_->now());
    trace_->emit(SimEventType::EPD_COMMITTED, buf, sched_->now());
    pending_.valid = false;
  }

  // Abort any in-flight refresh (device reboot / suppress). Returns the frame
  // that never made it to physical, if any. The scheduled completion callback
  // is invalidated via the refresh token, so the firmware is never told a
  // commit "completed" for a refresh that was aborted.
  SimFrame abortRefresh() {
    SimFrame lost = pending_;
    refreshToken_++;  // cancel any scheduled commit callback
    pending_.valid = false;
    busy_ = false;
    return lost;
  }

  static const char* modeName(RefreshMode m) {
    switch (m) {
      case RefreshMode::FAST_REFRESH: return "FAST";
      case RefreshMode::HALF_REFRESH: return "HALF";
      case RefreshMode::FULL_REFRESH: return "FULL";
      case RefreshMode::UI_FAST_REFRESH: return "UI_FAST";
    }
    return "?";
  }

private:
  SimScheduler* sched_;
  SimTrace* trace_;
  EpdProfile profile_;
  SimFrame renderFB_;
  SimFrame pending_;
  SimFrame physical_;
  bool busy_ = false;
  RefreshMode mode_ = RefreshMode::FAST_REFRESH;
  uint32_t refreshToken_ = 0;
};

}  // namespace m4sim

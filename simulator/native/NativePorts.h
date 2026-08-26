// Lightweight host/native backend for fast integration smoke tests.
//
// Unlike SimPanel/SimStorage, these classes deliberately do NOT model SSD1677
// timing, SD throughput or fault injection. They prove that ReaderModel is no
// longer coupled to the deterministic hardware model and give future native
// GfxRenderer/SDL/file-system adapters a concrete seam to replace.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "core/SimKernel.h"
#include "platform/DisplayPort.h"
#include "platform/StoragePort.h"

namespace m4native {

class NativeDisplay final : public m4platform::DisplayPort {
public:
  explicit NativeDisplay(m4sim::SimScheduler* sched, uint32_t commitDelayMs = 1)
      : sched_(sched), commitDelayMs_(commitDelayMs) {}

  bool busy() const override { return busy_; }

  void render(const m4platform::FrameTag& tag) override {
    rendered_ = tag;
    hasRendered_ = true;
  }

  bool submit(m4platform::RefreshMode /*mode*/,
              std::function<void()> onCommitted = nullptr,
              m4platform::RefreshContext /*context*/ = m4platform::RefreshContext::UI_CONTEXT) override {
    if (busy_ || !hasRendered_) return false;
    busy_ = true;
    pending_ = rendered_;
    hasRendered_ = false;
    sched_->scheduleIn(commitDelayMs_, [this, onCommitted]() {
      physical_ = pending_;
      hasPhysical_ = true;
      busy_ = false;
      if (onCommitted) onCommitted();
    });
    return true;
  }

  bool hasPhysicalFrame() const { return hasPhysical_; }
  const m4platform::FrameTag& physicalTag() const { return physical_; }

private:
  m4sim::SimScheduler* sched_;
  uint32_t commitDelayMs_ = 1;
  bool busy_ = false;
  bool hasRendered_ = false;
  bool hasPhysical_ = false;
  m4platform::FrameTag rendered_;
  m4platform::FrameTag pending_;
  m4platform::FrameTag physical_;
};

class NativeStorage final : public m4platform::StoragePort {
public:
  explicit NativeStorage(m4sim::SimScheduler* sched, uint32_t latencyMs = 1)
      : sched_(sched), latencyMs_(latencyMs) {}

  void read(const std::string& /*what*/, size_t bytes, double /*speedScale*/,
            std::function<void(size_t)> onDone) override {
    sched_->scheduleIn(latencyMs_, [bytes, onDone]() { onDone(bytes); });
  }

private:
  m4sim::SimScheduler* sched_;
  uint32_t latencyMs_ = 1;
};

}  // namespace m4native

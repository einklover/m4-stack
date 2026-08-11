// SimStorage: async SD model with per-operation latency, plus a slow-SD fault
// multiplier. Mirrors the firmware's SdFat-over-SPI cost profile.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "core/SimKernel.h"
#include "platform/StoragePort.h"

namespace m4sim {

class SimStorage final : public m4platform::StoragePort {
public:
  SimStorage(SimScheduler* sched, SimTrace* trace) : sched_(sched), trace_(trace) {}

  // Latency model (ms), based on typical SDHC SPI reads.
  static uint32_t readCostMs(size_t bytes, double speedScale = 1.0) {
    // ~8KB per 6ms bulk read; index/seek cost ~2ms fixed.
    double ms = 2.0 + (double)bytes / 8192.0 * 6.0;
    return (uint32_t)(ms * speedScale);
  }
  static uint32_t seekCostMs() { return 1; }

  // Async read: schedules completion after the modeled latency. `onDone` gets
  // the simulated return (bytes read, or 0 on fault).
  void read(const std::string& what, size_t bytes, double speedScale,
            std::function<void(size_t)> onDone) override {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s bytes=%zu speed=%.1fx", what.c_str(), bytes, speedScale);
    trace_->emit(SimEventType::SD_READ, buf, sched_->now());
    uint32_t cost = readCostMs(bytes, speedScale);
    uint32_t t0 = sched_->now();
    if (faultShortRead_) {
      faultShortRead_ = false;
      sched_->scheduleAt(t0 + 1, [onDone]() { onDone(0); });  // read returns 0
      return;
    }
    sched_->scheduleAt(t0 + cost, [onDone, bytes]() { onDone(bytes); });
  }

  void injectShortRead() { faultShortRead_ = true; }

private:
  SimScheduler* sched_;
  SimTrace* trace_;
  bool faultShortRead_ = false;
};

}  // namespace m4sim

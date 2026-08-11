#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/SimKernel.h"

namespace m4sim {

class SimSdmmc {
 public:
  struct Config {
    int clk = -1;
    int cmd = -1;
    int d0 = -1;
    int d1 = -1;
    int d2 = -1;
    int d3 = -1;
    unsigned busWidth = 4;
    uint32_t blockReadMs = 2;
  };

  explicit SimSdmmc(SimScheduler* sched, SimTrace* trace, Config cfg,
                    std::function<bool()> powered)
      : sched_(sched), trace_(trace), cfg_(cfg), powered_(std::move(powered)) {}

  void insert(size_t blocks = 32768) {
    inserted_ = true;
    blocks_ = blocks;
    generation_++;
  }

  void remove() {
    inserted_ = false;
    mounted_ = false;
    generation_++;
  }

  bool mount(unsigned requestedWidth = 4) {
    if (!powered_()) return fail("SDMMC mount while card rail off");
    if (!inserted_) return fail("SDMMC mount with no card");
    if (requestedWidth != cfg_.busWidth) return fail("SDMMC bus-width mismatch");
    mounted_ = true;
    emit("mount width=" + std::to_string(requestedWidth));
    return true;
  }

  void unmount() {
    mounted_ = false;
    generation_++;
    emit("unmount");
  }

  // Completion is asynchronous so power-loss/card-removal races are testable.
  bool readBlock(uint32_t lba, std::function<void(bool)> done) {
    if (!mounted_ || !powered_() || !inserted_) {
      fail("SDMMC read while unavailable");
      if (done) done(false);
      return false;
    }
    if (lba >= blocks_) {
      fail("SDMMC LBA out of range");
      if (done) done(false);
      return false;
    }
    const uint32_t op = ++readCount_;
    const uint32_t gen = generation_;
    const bool injected = failReadNumber_ != 0 && op == failReadNumber_;
    if (trace_) trace_->emit(SimEventType::SD_READ,
                             "begin lba=" + std::to_string(lba), now());
    auto finish = [this, lba, gen, injected, done]() {
      const bool ok = !injected && mounted_ && inserted_ && powered_() && gen == generation_;
      if (trace_) trace_->emit(SimEventType::SD_READ,
                               std::string("end lba=") + std::to_string(lba) +
                                   (ok ? " ok" : " failed"),
                               now());
      if (done) done(ok);
    };
    if (sched_) sched_->scheduleIn(cfg_.blockReadMs, finish);
    else finish();
    return true;
  }

  // Called by board power gate observers/tests after changing the rail. A power
  // interruption invalidates in-flight operations and requires a remount.
  void notifyPowerChanged() {
    if (!powered_()) {
      mounted_ = false;
      generation_++;
      emit("power_off invalidated mount");
    }
  }

  void failNthRead(uint32_t n) { failReadNumber_ = n; }
  bool mounted() const { return mounted_; }
  bool inserted() const { return inserted_; }
  uint32_t reads() const { return readCount_; }
  const std::vector<std::string>& errors() const { return errors_; }
  const Config& config() const { return cfg_; }

 private:
  uint32_t now() const { return sched_ ? sched_->now() : 0; }
  void emit(const std::string& msg) {
    if (trace_) trace_->emit(SimEventType::STATE_CHANGED, "SDMMC " + msg, now());
  }
  bool fail(const std::string& msg) {
    errors_.push_back(msg);
    if (trace_) trace_->emit(SimEventType::ASSERT, "SDMMC " + msg, now());
    return false;
  }

  SimScheduler* sched_ = nullptr;
  SimTrace* trace_ = nullptr;
  Config cfg_;
  std::function<bool()> powered_;
  bool inserted_ = false;
  bool mounted_ = false;
  size_t blocks_ = 0;
  uint32_t generation_ = 1;
  uint32_t readCount_ = 0;
  uint32_t failReadNumber_ = 0;
  std::vector<std::string> errors_;
};

}  // namespace m4sim

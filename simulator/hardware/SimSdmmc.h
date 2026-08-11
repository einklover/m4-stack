#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/SimKernel.h"
#include "hardware/SimSdCardImage.h"

namespace m4sim {

class SimSdmmc {
 public:
  using Block = SimSdCardImage::Block;

  struct Config {
    int clk = -1;
    int cmd = -1;
    int d0 = -1;
    int d1 = -1;
    int d2 = -1;
    int d3 = -1;
    unsigned busWidth = 4;
    uint32_t blockReadMs = 2;
    uint32_t blockWriteMs = 3;
  };

  explicit SimSdmmc(SimScheduler* sched, SimTrace* trace, Config cfg,
                    std::function<bool()> powered)
      : sched_(sched), trace_(trace), cfg_(cfg), powered_(std::move(powered)) {}

  // Compatibility helper for unit tests that only care about capacity/timing.
  // The card is nevertheless backed by real 512-byte sectors so the same model
  // can later feed filesystem-level fixtures.
  void insert(size_t blocks = 32768) {
    inserted_ = image_.create(blocks, 0x00);
    mounted_ = false;
    generation_++;
    emit("insert blocks=" + std::to_string(image_.blocks()));
  }

  bool insertImage(const std::string& path, bool writable = true) {
    if (!image_.load(path, writable)) return fail("SDMMC invalid card image: " + path);
    inserted_ = true;
    mounted_ = false;
    generation_++;
    emit("insert_image blocks=" + std::to_string(image_.blocks()));
    return true;
  }

  void remove() {
    inserted_ = false;
    mounted_ = false;
    generation_++;
    emit("remove");
  }

  bool mount(unsigned requestedWidth = 4) {
    if (!powered_()) return fail("SDMMC mount while card rail off");
    if (!inserted_) return fail("SDMMC mount with no card");
    if (image_.empty()) return fail("SDMMC mount with empty backing image");
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

  // Compatibility API: asynchronous success/failure without exposing payload.
  bool readBlock(uint32_t lba, std::function<void(bool)> done) {
    return readBlockData(lba, [done = std::move(done)](bool ok, const Block&) {
      if (done) done(ok);
    });
  }

  // Completion is asynchronous so power-loss/card-removal races remain
  // testable. Data is sampled at completion, matching the operation generation.
  bool readBlockData(uint32_t lba, std::function<void(bool, const Block&)> done) {
    Block empty{};
    if (!availableForIo()) {
      fail("SDMMC read while unavailable");
      if (done) done(false, empty);
      return false;
    }
    if (lba >= image_.blocks()) {
      fail("SDMMC LBA out of range");
      if (done) done(false, empty);
      return false;
    }
    const uint32_t op = ++readCount_;
    const uint32_t gen = generation_;
    const bool injected = failReadNumber_ != 0 && op == failReadNumber_;
    if (trace_) trace_->emit(SimEventType::SD_READ,
                             "begin lba=" + std::to_string(lba), now());
    auto finish = [this, lba, gen, injected, done = std::move(done)]() {
      Block block{};
      const bool stateOk = !injected && availableForIo() && gen == generation_;
      const bool ok = stateOk && image_.readBlock(lba, block);
      if (trace_) trace_->emit(SimEventType::SD_READ,
                               std::string("end lba=") + std::to_string(lba) +
                                   (ok ? " ok" : " failed"),
                               now());
      if (done) done(ok, block);
    };
    if (sched_) sched_->scheduleIn(cfg_.blockReadMs, std::move(finish));
    else finish();
    return true;
  }

  bool writeBlockData(uint32_t lba, const Block& block, std::function<void(bool)> done = {}) {
    if (!availableForIo()) {
      fail("SDMMC write while unavailable");
      if (done) done(false);
      return false;
    }
    if (!image_.writable()) {
      fail("SDMMC write to read-only card image");
      if (done) done(false);
      return false;
    }
    if (lba >= image_.blocks()) {
      fail("SDMMC write LBA out of range");
      if (done) done(false);
      return false;
    }
    const uint32_t op = ++writeCount_;
    const uint32_t gen = generation_;
    const bool injected = failWriteNumber_ != 0 && op == failWriteNumber_;
    if (trace_) trace_->emit(SimEventType::STATE_CHANGED,
                             "SDMMC write begin lba=" + std::to_string(lba), now());
    auto finish = [this, lba, block, gen, injected, done = std::move(done)]() {
      const bool stateOk = !injected && availableForIo() && gen == generation_;
      const bool ok = stateOk && image_.writeBlock(lba, block);
      if (trace_) trace_->emit(SimEventType::STATE_CHANGED,
                               std::string("SDMMC write end lba=") + std::to_string(lba) +
                                   (ok ? " ok" : " failed"),
                               now());
      if (done) done(ok);
    };
    if (sched_) sched_->scheduleIn(cfg_.blockWriteMs, std::move(finish));
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

  bool flushImage(const std::string& path = std::string()) { return image_.save(path); }

  void failNthRead(uint32_t n) { failReadNumber_ = n; }
  void failNthWrite(uint32_t n) { failWriteNumber_ = n; }
  bool mounted() const { return mounted_; }
  bool inserted() const { return inserted_; }
  bool cardDetectAsserted() const { return inserted_; }
  uint32_t reads() const { return readCount_; }
  uint32_t writes() const { return writeCount_; }
  std::size_t blocks() const { return image_.blocks(); }
  SimSdCardImage& image() { return image_; }
  const SimSdCardImage& image() const { return image_; }
  const std::vector<std::string>& errors() const { return errors_; }
  const Config& config() const { return cfg_; }

 private:
  bool availableForIo() const { return mounted_ && powered_() && inserted_; }
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
  SimSdCardImage image_;
  bool inserted_ = false;
  bool mounted_ = false;
  uint32_t generation_ = 1;
  uint32_t readCount_ = 0;
  uint32_t writeCount_ = 0;
  uint32_t failReadNumber_ = 0;
  uint32_t failWriteNumber_ = 0;
  std::vector<std::string> errors_;
};

}  // namespace m4sim

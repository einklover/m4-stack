#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "board/MurphyM4Spec.h"
#include "core/SimKernel.h"
#include "hardware/SimSpiBus.h"

namespace m4sim {

// Register/RAM-level SSD1677 model for driver and board-contract testing.
//
// This model is intentionally below SimPanel: it consumes the same command/data
// stream as FreeInk's Ssd1677Driver. It models controller RAM, update selection,
// BUSY and activation/commit atomicity. It DOES NOT claim to model electrophoretic
// particle physics, temperature-dependent ghosting, or analog gray levels.
class SimSsd1677Controller final : public SimSpiSink {
 public:
  struct Timing {
    uint32_t resetMs = 10;
    uint32_t fastMs = 420;
    uint32_t halfMs = 850;
    uint32_t fullMs = 1800;
    uint32_t powerMs = 30;
  };

  SimSsd1677Controller(SimScheduler* sched, SimTrace* trace)
      : SimSsd1677Controller(sched, trace, Timing{10, 420, 850, 1800, 30}) {}

  SimSsd1677Controller(SimScheduler* sched, SimTrace* trace, Timing timing)
      : sched_(sched), trace_(trace), timing_(timing),
        bwRam_(m4board::MurphyM4Spec::kFramebufferBytes, 0xFF),
        redRam_(m4board::MurphyM4Spec::kFramebufferBytes, 0xFF),
        physical_(m4board::MurphyM4Spec::kFramebufferBytes, 0xFF),
        pending_(m4board::MurphyM4Spec::kFramebufferBytes, 0xFF) {
    resetWindow();
  }

  bool transfer(bool dataMode, const uint8_t* bytes, size_t count) override {
    if (!dataMode) {
      if (count != 1) return fail("SSD1677 command transfer must contain exactly one byte");
      return command(bytes[0]);
    }
    return data(bytes, count);
  }

  bool command(uint8_t cmd) {
    if (expectingData_ && receivedForCommand_ < expectedForCommand_) {
      fail("SSD1677 command replaced before required data completed for 0x" + hex(lastCommand_));
    }
    lastCommand_ = cmd;
    receivedForCommand_ = 0;
    expectedForCommand_ = expectedDataBytes(cmd);
    expectingData_ = expectedForCommand_ != 0 || cmd == kWriteBw || cmd == kWriteRed;
    commandData_.clear();

    if (cmd == kSoftReset) {
      hardReset(/*preservePhysical=*/true);
      startBusy(timing_.resetMs, "soft_reset", false);
    } else if (cmd == kMasterActivation) {
      activate();
    } else if (cmd == kDeepSleep) {
      sleeping_ = true;
      powered_ = false;
      emit("deep_sleep");
    }
    return errors_.empty();
  }

  bool data(const uint8_t* bytes, size_t count) {
    if (!bytes && count) return fail("SSD1677 null data buffer");
    if (!expectingData_) return fail("SSD1677 data with no data-accepting command");

    if (lastCommand_ == kWriteBw || lastCommand_ == kWriteRed) {
      return ramData(lastCommand_, bytes, count);
    }

    if (expectedForCommand_ == 0) return fail("SSD1677 command does not accept data");
    if (receivedForCommand_ + count > expectedForCommand_) {
      return fail("SSD1677 too much data for command 0x" + hex(lastCommand_));
    }
    commandData_.insert(commandData_.end(), bytes, bytes + count);
    receivedForCommand_ += count;
    if (receivedForCommand_ == expectedForCommand_) {
      expectingData_ = false;
      applyRegisterData(lastCommand_, commandData_);
    }
    return errors_.empty();
  }

  void hardReset(bool preservePhysical = true) {
    busy_ = false;
    busyToken_++;
    sleeping_ = false;
    powered_ = false;
    customLutLoaded_ = false;
    updateCtrl1_ = 0;
    updateCtrl2_ = 0;
    dataEntryMode_ = 0x01;
    std::fill(bwRam_.begin(), bwRam_.end(), 0xFF);
    std::fill(redRam_.begin(), redRam_.end(), 0xFF);
    if (!preservePhysical) std::fill(physical_.begin(), physical_.end(), 0xFF);
    pending_ = physical_;
    resetWindow();
    expectingData_ = false;
    expectedForCommand_ = receivedForCommand_ = 0;
    ramReceived_ = 0;
    emit("reset");
  }

  bool busy() const { return busy_; }
  bool powered() const { return powered_; }
  bool sleeping() const { return sleeping_; }
  bool customLutLoaded() const { return customLutLoaded_; }
  uint8_t updateCtrl1() const { return updateCtrl1_; }
  uint8_t updateCtrl2() const { return updateCtrl2_; }
  uint32_t activationCount() const { return activations_; }
  uint32_t commitCount() const { return commits_; }
  size_t lastChangedBits() const { return lastChangedBits_; }
  size_t lastRamExpected() const { return ramExpected_; }
  const std::vector<uint8_t>& bwRam() const { return bwRam_; }
  const std::vector<uint8_t>& redRam() const { return redRam_; }
  const std::vector<uint8_t>& physical() const { return physical_; }
  const std::vector<std::string>& errors() const { return errors_; }
  bool ok() const { return errors_.empty(); }

  // For issue #1 / waveform tests: an activation has exactly one externally
  // observable commit boundary in this digital controller model. A multi-step
  // wipe therefore requires multiple activations/windows; LUT analog evolution
  // within one activation is deliberately outside this model's fidelity.
  bool activationCommitsAtomically() const { return true; }

 private:
  static constexpr uint8_t kSoftReset = 0x12;
  static constexpr uint8_t kBooster = 0x0C;
  static constexpr uint8_t kDriverOutput = 0x01;
  static constexpr uint8_t kBorder = 0x3C;
  static constexpr uint8_t kTempSensor = 0x18;
  static constexpr uint8_t kDataEntry = 0x11;
  static constexpr uint8_t kSetXRange = 0x44;
  static constexpr uint8_t kSetYRange = 0x45;
  static constexpr uint8_t kSetXCounter = 0x4E;
  static constexpr uint8_t kSetYCounter = 0x4F;
  static constexpr uint8_t kWriteBw = 0x24;
  static constexpr uint8_t kWriteRed = 0x26;
  static constexpr uint8_t kAutoBw = 0x46;
  static constexpr uint8_t kAutoRed = 0x47;
  static constexpr uint8_t kUpdateCtrl1 = 0x21;
  static constexpr uint8_t kUpdateCtrl2 = 0x22;
  static constexpr uint8_t kMasterActivation = 0x20;
  static constexpr uint8_t kWriteLut = 0x32;
  static constexpr uint8_t kGateVoltage = 0x03;
  static constexpr uint8_t kSourceVoltage = 0x04;
  static constexpr uint8_t kWriteVcom = 0x2C;
  static constexpr uint8_t kWriteTemp = 0x1A;
  static constexpr uint8_t kDeepSleep = 0x10;

  static size_t expectedDataBytes(uint8_t cmd) {
    switch (cmd) {
      case kBooster: return 5;
      case kDriverOutput: return 3;
      case kBorder:
      case kTempSensor:
      case kDataEntry:
      case kAutoBw:
      case kAutoRed:
      case kUpdateCtrl1:
      case kUpdateCtrl2:
      case kGateVoltage:
      case kWriteVcom:
      case kWriteTemp:
      case kDeepSleep: return 1;
      case kSetXRange:
      case kSetYRange: return 4;
      case kSetXCounter:
      case kSetYCounter: return 2;
      case kWriteLut: return 105;
      case kSourceVoltage: return 3;
      default: return 0;
    }
  }

  static std::string hex(uint8_t v) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned>(v));
    return buf;
  }

  static uint16_t le16(const std::vector<uint8_t>& d, size_t i) {
    return static_cast<uint16_t>(d[i]) | (static_cast<uint16_t>(d[i + 1]) << 8);
  }

  void applyRegisterData(uint8_t cmd, const std::vector<uint8_t>& d) {
    switch (cmd) {
      case kDataEntry:
        dataEntryMode_ = d[0];
        break;
      case kSetXRange:
        xStart_ = le16(d, 0);
        xEnd_ = le16(d, 2);
        updateRamExpected();
        break;
      case kSetYRange:
        yStart_ = le16(d, 0);
        yEnd_ = le16(d, 2);
        updateRamExpected();
        break;
      case kSetXCounter:
        xCounter_ = le16(d, 0);
        break;
      case kSetYCounter:
        yCounter_ = le16(d, 0);
        break;
      case kUpdateCtrl1:
        updateCtrl1_ = d[0];
        break;
      case kUpdateCtrl2:
        updateCtrl2_ = d[0];
        break;
      case kWriteLut:
        customLutLoaded_ = true;
        lutHash_ = fnv1a(d);
        break;
      case kDeepSleep:
        sleeping_ = true;
        powered_ = false;
        break;
      default:
        break;
    }
  }

  void resetWindow() {
    xStart_ = 0;
    xEnd_ = m4board::MurphyM4Spec::kDisplayWidth - 1;
    yStart_ = m4board::MurphyM4Spec::kDisplayHeight - 1;
    yEnd_ = 0;
    xCounter_ = xStart_;
    yCounter_ = yStart_;
    updateRamExpected();
  }

  void updateRamExpected() {
    const uint16_t xLo = std::min(xStart_, xEnd_);
    const uint16_t xHi = std::max(xStart_, xEnd_);
    const uint16_t yLo = std::min(yStart_, yEnd_);
    const uint16_t yHi = std::max(yStart_, yEnd_);
    if (xHi >= m4board::MurphyM4Spec::kDisplayWidth ||
        yHi >= m4board::MurphyM4Spec::kDisplayHeight || xLo > xHi || yLo > yHi) {
      ramExpected_ = 0;
      fail("SSD1677 RAM window outside 800x480 geometry");
      return;
    }
    const size_t width = static_cast<size_t>(xHi - xLo + 1);
    const size_t height = static_cast<size_t>(yHi - yLo + 1);
    if (width % 8 != 0) {
      ramExpected_ = 0;
      fail("SSD1677 RAM window width is not byte aligned");
      return;
    }
    ramExpected_ = width / 8 * height;
  }

  bool ramData(uint8_t cmd, const uint8_t* bytes, size_t count) {
    if (ramReceived_ == 0) {
      ramTarget_ = cmd;
      if (ramExpected_ == 0) return fail("SSD1677 RAM write with invalid window");
      ramStaging_.clear();
      ramStaging_.reserve(ramExpected_);
    } else if (ramTarget_ != cmd) {
      return fail("SSD1677 RAM plane changed mid-write");
    }
    if (ramReceived_ + count > ramExpected_) {
      return fail("SSD1677 RAM write exceeds active window");
    }
    ramStaging_.insert(ramStaging_.end(), bytes, bytes + count);
    ramReceived_ += count;
    if (ramReceived_ == ramExpected_) {
      applyWindow(cmd == kWriteBw ? bwRam_ : redRam_, ramStaging_);
      ramReceived_ = 0;
      expectingData_ = false;
    }
    return errors_.empty();
  }

  void applyWindow(std::vector<uint8_t>& plane, const std::vector<uint8_t>& src) {
    const uint16_t xLo = std::min(xStart_, xEnd_);
    const uint16_t xHi = std::max(xStart_, xEnd_);
    const uint16_t yLo = std::min(yStart_, yEnd_);
    const uint16_t yHi = std::max(yStart_, yEnd_);
    const size_t rowBytes = (xHi - xLo + 1) / 8;
    const size_t fullRowBytes = m4board::MurphyM4Spec::kDisplayWidth / 8;
    size_t at = 0;
    for (uint16_t y = yLo; y <= yHi; ++y) {
      const size_t dst = static_cast<size_t>(y) * fullRowBytes + xLo / 8;
      std::copy(src.begin() + static_cast<std::ptrdiff_t>(at),
                src.begin() + static_cast<std::ptrdiff_t>(at + rowBytes),
                plane.begin() + static_cast<std::ptrdiff_t>(dst));
      at += rowBytes;
    }
  }

  void activate() {
    if (busy_) {
      fail("SSD1677 MASTER_ACTIVATION while BUSY");
      return;
    }
    if (sleeping_) {
      fail("SSD1677 MASTER_ACTIVATION while in deep sleep");
      return;
    }
    activations_++;
    pending_ = bwRam_;
    lastChangedBits_ = diffBits(bwRam_, redRam_);
    const uint32_t duration = activationDuration(updateCtrl2_);
    // 0x22 can contain clock/analog on/off bits. For digital simulation, any
    // activation with ON bits raises the logical rail; OFF bits clear it after
    // completion. The actual analog ramp remains hardware-calibrated territory.
    if (updateCtrl2_ & 0xC0) powered_ = true;
    const bool turnOff = (updateCtrl2_ & 0x03) != 0;
    char msg[128];
    snprintf(msg, sizeof(msg), "ctrl2=0x%02x diff_bits=%zu duration=%u", updateCtrl2_,
             lastChangedBits_, duration);
    if (trace_) trace_->emit(SimEventType::EPD_SUBMITTED, msg, now());
    startBusy(duration, "activation", true, turnOff);
  }

  uint32_t activationDuration(uint8_t seq) const {
    if (seq == 0xC0) return timing_.powerMs;
    if (seq == 0xF7 || seq == 0x34 || seq == 0xC7) return timing_.fullMs;
    if (seq == 0xD7 || seq == 0xD4) return timing_.halfMs;
    return timing_.fastMs;
  }

  void startBusy(uint32_t duration, const char* why, bool commit, bool turnOff = false) {
    busy_ = true;
    const uint32_t token = ++busyToken_;
    if (trace_) trace_->emit(SimEventType::EPD_BUSY, why, now());
    if (!sched_) {
      finishBusy(token, commit, turnOff);
      return;
    }
    sched_->scheduleIn(duration, [this, token, commit, turnOff]() {
      finishBusy(token, commit, turnOff);
    });
  }

  void finishBusy(uint32_t token, bool commit, bool turnOff) {
    if (token != busyToken_) return;
    busy_ = false;
    if (commit) {
      physical_ = pending_;
      commits_++;
      if (trace_) trace_->emit(SimEventType::EPD_COMMITTED,
                               "ssd1677 activation committed atomically", now());
    }
    if (turnOff) powered_ = false;
  }

  uint32_t now() const { return sched_ ? sched_->now() : 0; }

  void emit(const std::string& msg) {
    if (trace_) trace_->emit(SimEventType::STATE_CHANGED, "SSD1677 " + msg, now());
  }

  bool fail(const std::string& msg) {
    errors_.push_back(msg);
    if (trace_) trace_->emit(SimEventType::ASSERT, "SSD1677 protocol: " + msg, now());
    return false;
  }

  static uint64_t fnv1a(const std::vector<uint8_t>& v) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : v) {
      h ^= b;
      h *= 1099511628211ull;
    }
    return h;
  }

  static size_t diffBits(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      uint8_t x = static_cast<uint8_t>(a[i] ^ b[i]);
      while (x) {
        n += x & 1u;
        x >>= 1;
      }
    }
    return n;
  }

  SimScheduler* sched_ = nullptr;
  SimTrace* trace_ = nullptr;
  Timing timing_;
  std::vector<uint8_t> bwRam_;
  std::vector<uint8_t> redRam_;
  std::vector<uint8_t> physical_;
  std::vector<uint8_t> pending_;
  std::vector<uint8_t> commandData_;
  std::vector<uint8_t> ramStaging_;
  std::vector<std::string> errors_;

  uint8_t lastCommand_ = 0;
  size_t expectedForCommand_ = 0;
  size_t receivedForCommand_ = 0;
  bool expectingData_ = false;
  uint8_t ramTarget_ = 0;
  size_t ramExpected_ = m4board::MurphyM4Spec::kFramebufferBytes;
  size_t ramReceived_ = 0;

  uint16_t xStart_ = 0, xEnd_ = 799, yStart_ = 479, yEnd_ = 0;
  uint16_t xCounter_ = 0, yCounter_ = 479;
  uint8_t dataEntryMode_ = 0x01;
  uint8_t updateCtrl1_ = 0;
  uint8_t updateCtrl2_ = 0;
  uint64_t lutHash_ = 0;
  bool customLutLoaded_ = false;
  bool sleeping_ = false;
  bool powered_ = false;
  bool busy_ = false;
  uint32_t busyToken_ = 0;
  uint32_t activations_ = 0;
  uint32_t commits_ = 0;
  size_t lastChangedBits_ = 0;
};

}  // namespace m4sim

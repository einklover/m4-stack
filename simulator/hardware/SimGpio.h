#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace m4sim {

enum class GpioMode { Unconfigured, Input, Output, OpenDrain };
enum class GpioPull { None, Up, Down };

class SimGpio {
 public:
  static constexpr int kMaxPin = 48;

  struct PinState {
    GpioMode mode = GpioMode::Unconfigured;
    GpioPull pull = GpioPull::None;
    bool outputLevel = false;
    std::optional<bool> externalLevel;
    std::string owner;
    bool shareable = false;
  };

  bool claim(int pin, const std::string& owner, bool shareable = false) {
    if (!valid(pin)) return fail("claim invalid GPIO" + std::to_string(pin));
    auto& p = pins_[pin];
    if (!p.owner.empty() && p.owner != owner && !(p.shareable && shareable)) {
      return fail("GPIO" + std::to_string(pin) + " owner conflict: " + p.owner + " vs " + owner);
    }
    if (p.owner.empty()) p.owner = owner;
    p.shareable = p.shareable || shareable;
    return true;
  }

  bool configure(int pin, GpioMode mode, GpioPull pull = GpioPull::None) {
    if (!valid(pin)) return fail("configure invalid GPIO" + std::to_string(pin));
    pins_[pin].mode = mode;
    pins_[pin].pull = pull;
    return true;
  }

  bool write(int pin, bool level) {
    if (!valid(pin)) return fail("write invalid GPIO" + std::to_string(pin));
    auto& p = pins_[pin];
    if (p.mode != GpioMode::Output && p.mode != GpioMode::OpenDrain) {
      return fail("write GPIO" + std::to_string(pin) + " while not output");
    }
    p.outputLevel = level;
    return true;
  }

  bool driveExternal(int pin, bool level) {
    if (!valid(pin)) return fail("drive invalid GPIO" + std::to_string(pin));
    auto& p = pins_[pin];
    if (p.mode == GpioMode::Output && p.outputLevel != level) {
      fail("electrical contention on GPIO" + std::to_string(pin));
    }
    p.externalLevel = level;
    return violations_.empty();
  }

  void releaseExternal(int pin) {
    if (valid(pin)) pins_[pin].externalLevel.reset();
  }

  bool read(int pin) const {
    if (!valid(pin)) return false;
    const auto& p = pins_[pin];
    if (p.mode == GpioMode::Output) return p.outputLevel;
    if (p.mode == GpioMode::OpenDrain && !p.outputLevel) return false;
    if (p.externalLevel.has_value()) return *p.externalLevel;
    if (p.pull == GpioPull::Up) return true;
    if (p.pull == GpioPull::Down) return false;
    return p.outputLevel;  // deterministic floating default
  }

  const PinState& state(int pin) const { return pins_.at(static_cast<size_t>(pin)); }
  const std::vector<std::string>& violations() const { return violations_; }
  bool ok() const { return violations_.empty(); }
  void clearViolations() { violations_.clear(); }

 private:
  static bool valid(int pin) { return pin >= 0 && pin <= kMaxPin; }
  bool fail(const std::string& msg) {
    violations_.push_back(msg);
    return false;
  }

  std::array<PinState, kMaxPin + 1> pins_{};
  std::vector<std::string> violations_;
};

}  // namespace m4sim

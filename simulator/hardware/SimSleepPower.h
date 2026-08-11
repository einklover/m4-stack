#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "board/MurphyM4Spec.h"
#include "hardware/SimGpio.h"
#include "hardware/SimPower.h"

namespace m4sim {

// Mirrors the FreeInk PowerManager semantics relevant to Murphy M4:
// - power/lock key GPIO0 is active-low and held INPUT_PULLUP in sleep;
// - ESP32-S3 ext1 wakes on ANY_LOW for that pin;
// - SD/touch active-low enables are driven HIGH and held through deep sleep;
// - deep sleep never returns; simulation represents the next boot as wake().
class SimSleepPower {
 public:
  enum class WakeReason { None, PowerButton, External, Reset };

  SimSleepPower(SimGpio* gpio, SimPowerGate* sd, SimPowerGate* touch)
      : gpio_(gpio), sd_(sd), touch_(touch) {}

  bool enterDeepSleep() {
    if (!gpio_ || !sd_ || !touch_) return fail("sleep dependencies missing");
    // Production waits until the button is released before arming active-low
    // wake. Entering with the line held low would immediately re-wake.
    if (!gpio_->read(m4board::MurphyM4Spec::kKeyLockPower)) {
      return fail("power button still pressed while arming deep sleep");
    }
    gpio_->configure(m4board::MurphyM4Spec::kKeyLockPower,
                     GpioMode::Input, GpioPull::Up);
    sd_->setPowered(false);
    touch_->setPowered(false);
    sleeping_ = true;
    wakeReason_ = WakeReason::None;
    railsHeld_ = true;
    return true;
  }

  bool injectPowerButton(bool pressed) {
    if (!gpio_) return false;
    // Active-low physical key.
    gpio_->driveExternal(m4board::MurphyM4Spec::kKeyLockPower, !pressed);
    if (sleeping_ && pressed) {
      sleeping_ = false;
      wakeReason_ = WakeReason::PowerButton;
      // GPIO deep-sleep holds survive reset until each driver explicitly
      // releases/reconfigures its rail. Keep logical rails off here.
      return true;
    }
    return false;
  }

  void releaseHeldRailsForBoot() { railsHeld_ = false; }
  bool sleeping() const { return sleeping_; }
  bool railsHeld() const { return railsHeld_; }
  WakeReason wakeReason() const { return wakeReason_; }
  const std::vector<std::string>& errors() const { return errors_; }

 private:
  bool fail(const std::string& msg) {
    errors_.push_back(msg);
    return false;
  }

  SimGpio* gpio_ = nullptr;
  SimPowerGate* sd_ = nullptr;
  SimPowerGate* touch_ = nullptr;
  bool sleeping_ = false;
  bool railsHeld_ = false;
  WakeReason wakeReason_ = WakeReason::None;
  std::vector<std::string> errors_;
};

}  // namespace m4sim

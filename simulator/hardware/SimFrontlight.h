#pragma once

#include <algorithm>
#include <cstdint>

namespace m4sim {

struct FrontlightDuty {
  uint32_t cool = 0;
  uint32_t warm = 0;
};

// Mirrors FreeInk FrontlightManager dual-channel math. Overall brightness is
// split between cool/warm by warmPercent so color-temperature changes do not
// double total light output.
class SimFrontlight {
 public:
  SimFrontlight(uint32_t frequencyHz = 5000, uint8_t resolutionBits = 8,
                bool activeHigh = true)
      : frequencyHz_(frequencyHz), resolutionBits_(resolutionBits), activeHigh_(activeHigh) {}

  void setBrightness(uint8_t percent) {
    brightness_ = std::min<uint8_t>(percent, 100);
    if (brightness_ > 0) lastBrightness_ = brightness_;
    recompute();
  }

  void setWarmPercent(uint8_t percent) {
    warmPercent_ = std::min<uint8_t>(percent, 100);
    recompute();
  }

  void off() { setBrightness(0); }
  void on() { setBrightness(lastBrightness_); }

  uint8_t brightness() const { return brightness_; }
  uint8_t warmPercent() const { return warmPercent_; }
  uint8_t lastBrightness() const { return lastBrightness_; }
  uint32_t frequencyHz() const { return frequencyHz_; }
  uint8_t resolutionBits() const { return resolutionBits_; }
  FrontlightDuty duty() const { return duty_; }

 private:
  uint32_t maxDuty() const { return (1u << resolutionBits_) - 1u; }
  uint32_t dutyFor(uint32_t pct) const {
    const uint32_t full = maxDuty();
    const uint32_t raw = (pct * full) / 100u;
    return activeHigh_ ? raw : full - raw;
  }
  void recompute() {
    const uint32_t coolPct =
        (static_cast<uint32_t>(brightness_) * (100u - warmPercent_)) / 100u;
    const uint32_t warmPct =
        (static_cast<uint32_t>(brightness_) * warmPercent_) / 100u;
    duty_.cool = dutyFor(coolPct);
    duty_.warm = dutyFor(warmPct);
  }

  uint32_t frequencyHz_ = 5000;
  uint8_t resolutionBits_ = 8;
  bool activeHigh_ = true;
  uint8_t brightness_ = 0;
  uint8_t lastBrightness_ = 100;
  uint8_t warmPercent_ = 0;
  FrontlightDuty duty_{};
};

}  // namespace m4sim

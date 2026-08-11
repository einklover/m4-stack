#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace m4sim {

// Murphy M4 ADC battery backend mirrors the production BatteryMonitor policy:
// analogReadMilliVolts(GPIO9) is the divider-side voltage; BoardConfig multiplier
// 2.0 converts it to true cell mV; percentage uses the production cubic curve.
class SimBattery {
 public:
  explicit SimBattery(float dividerMultiplier = 2.0f)
      : dividerMultiplier_(dividerMultiplier) {}

  void setCellMillivolts(uint16_t mv) { cellMillivolts_ = mv; }
  uint16_t cellMillivolts() const { return cellMillivolts_; }

  uint16_t adcMillivolts() const {
    if (dividerMultiplier_ <= 0.0f) return 0;
    return static_cast<uint16_t>(std::lround(cellMillivolts_ / dividerMultiplier_));
  }

  uint16_t productionReadMillivolts() const {
    return static_cast<uint16_t>(adcMillivolts() * dividerMultiplier_);
  }

  uint16_t percentage() const { return percentageFromMillivolts(productionReadMillivolts()); }

  static uint16_t percentageFromMillivolts(uint16_t millivolts) {
    const double volts = millivolts / 1000.0;
    double y = -144.9390 * volts * volts * volts +
               1655.8629 * volts * volts -
               6158.8520 * volts +
               7501.3202;
    y = std::max(y, 0.0);
    y = std::min(y, 100.0);
    return static_cast<uint16_t>(std::round(y));
  }

 private:
  float dividerMultiplier_ = 2.0f;
  uint16_t cellMillivolts_ = 3800;
};

}  // namespace m4sim

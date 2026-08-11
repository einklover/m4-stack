#pragma once

#include <string>

#include "hardware/SimGpio.h"

namespace m4sim {

class SimPowerGate {
 public:
  SimPowerGate(SimGpio* gpio, int pin, bool activeHigh, std::string name)
      : gpio_(gpio), pin_(pin), activeHigh_(activeHigh), name_(std::move(name)) {
    if (gpio_) {
      gpio_->claim(pin_, name_ + ".power");
      gpio_->configure(pin_, GpioMode::Output);
      setPowered(false);
    }
  }

  void setPowered(bool on) {
    requestedOn_ = on;
    if (gpio_) gpio_->write(pin_, on ? activeHigh_ : !activeHigh_);
  }

  bool powered() const {
    if (!gpio_) return false;
    return gpio_->read(pin_) == activeHigh_;
  }

  bool requestedOn() const { return requestedOn_; }
  int pin() const { return pin_; }
  bool activeHigh() const { return activeHigh_; }
  const std::string& name() const { return name_; }

 private:
  SimGpio* gpio_ = nullptr;
  int pin_ = -1;
  bool activeHigh_ = true;
  std::string name_;
  bool requestedOn_ = false;
};

}  // namespace m4sim

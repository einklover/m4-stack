#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hardware/SimI2cBus.h"

namespace m4sim {

// FT6336/FT6x36 subset used by Murphy InputManager. The production driver writes
// register 0 with a STOP, delays 2ms, then reads an 8-byte frame. This device
// models the register/frame bytes and injected malformed/bus-failure cases; bus
// STOP timing remains an I2C-host integration concern.
class SimFt6x36Device final : public SimI2cDevice {
 public:
  enum class Event : uint8_t { Down = 0, Up = 1, Contact = 2 };

  bool write(const std::vector<uint8_t>& bytes) override {
    if (bytes.size() != 1) return false;
    selectedRegister_ = bytes[0];
    return true;
  }

  bool read(size_t count, std::vector<uint8_t>& out) override {
    if (failReads_ > 0) {
      --failReads_;
      out.clear();
      return false;
    }
    if (selectedRegister_ != 0 || count != 8) {
      out.clear();
      return false;
    }
    out.assign(frame_, frame_ + 8);
    return true;
  }

  void release() {
    clearFrame();
    frame_[2] = 0;  // no points
  }

  bool setPoint(uint16_t rawX, uint16_t rawY, Event event = Event::Contact,
                uint8_t points = 1) {
    if (rawX >= 480 || rawY >= 800 || points > 2) return false;
    clearFrame();
    frame_[2] = points & 0x0F;
    frame_[3] = static_cast<uint8_t>((static_cast<uint8_t>(event) << 6) |
                                     ((rawX >> 8) & 0x0F));
    frame_[4] = static_cast<uint8_t>(rawX & 0xFF);
    frame_[5] = static_cast<uint8_t>((rawY >> 8) & 0x0F);
    frame_[6] = static_cast<uint8_t>(rawY & 0xFF);
    return true;
  }

  void setMalformedRaw(uint16_t rawX, uint16_t rawY, Event event = Event::Contact) {
    clearFrame();
    frame_[2] = 1;
    frame_[3] = static_cast<uint8_t>((static_cast<uint8_t>(event) << 6) |
                                     ((rawX >> 8) & 0x0F));
    frame_[4] = static_cast<uint8_t>(rawX & 0xFF);
    frame_[5] = static_cast<uint8_t>((rawY >> 8) & 0x0F);
    frame_[6] = static_cast<uint8_t>(rawY & 0xFF);
  }

  void failNextReads(unsigned n) { failReads_ = n; }
  const uint8_t* frame() const { return frame_; }

 private:
  void clearFrame() {
    for (auto& b : frame_) b = 0;
  }

  uint8_t selectedRegister_ = 0;
  uint8_t frame_[8]{};
  unsigned failReads_ = 0;
};

}  // namespace m4sim

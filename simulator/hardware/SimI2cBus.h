#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace m4sim {

class SimI2cDevice {
 public:
  virtual ~SimI2cDevice() = default;
  virtual bool write(const std::vector<uint8_t>& bytes) = 0;
  virtual bool read(size_t count, std::vector<uint8_t>& out) = 0;
};

// Small register-style device useful for touch/sensor bring-up tests. It is not
// intended to fake a full controller protocol; dedicated models can implement
// SimI2cDevice when exact register semantics matter.
class SimRegisterI2cDevice : public SimI2cDevice {
 public:
  explicit SimRegisterI2cDevice(size_t bytes = 256) : regs_(bytes, 0) {}

  bool write(const std::vector<uint8_t>& bytes) override {
    if (bytes.empty()) return false;
    cursor_ = bytes[0];
    for (size_t i = 1; i < bytes.size(); ++i) {
      if (cursor_ >= regs_.size()) return false;
      regs_[cursor_++] = bytes[i];
    }
    return true;
  }

  bool read(size_t count, std::vector<uint8_t>& out) override {
    out.clear();
    for (size_t i = 0; i < count; ++i) {
      if (cursor_ >= regs_.size()) return false;
      out.push_back(regs_[cursor_++]);
    }
    return true;
  }

  void set(uint8_t reg, uint8_t value) {
    if (reg < regs_.size()) regs_[reg] = value;
  }

 private:
  std::vector<uint8_t> regs_;
  size_t cursor_ = 0;
};

class SimI2cBus {
 public:
  struct Entry {
    std::string name;
    SimI2cDevice* device = nullptr;
    std::function<bool()> powered;
    bool firmwareEnabled = true;
  };

  SimI2cBus(int sda, int scl, uint32_t hz = 400000)
      : sda_(sda), scl_(scl), hz_(hz) {}

  bool attach(uint8_t address, std::string name, SimI2cDevice* device,
              std::function<bool()> powered = [] { return true; },
              bool firmwareEnabled = true) {
    if (entries_.count(address)) {
      errors_.push_back("I2C address collision at 0x" + hex(address));
      return false;
    }
    entries_.emplace(address, Entry{std::move(name), device, std::move(powered), firmwareEnabled});
    return true;
  }

  bool probe(uint8_t address, bool firmwareView = true) {
    const auto it = entries_.find(address);
    if (it == entries_.end()) {
      trace_.push_back("probe 0x" + hex(address) + " NACK:no-device");
      return false;
    }
    const Entry& e = it->second;
    if (firmwareView && !e.firmwareEnabled) {
      trace_.push_back("probe 0x" + hex(address) + " hidden:firmware-disabled " + e.name);
      return false;
    }
    if (!e.powered()) {
      trace_.push_back("probe 0x" + hex(address) + " NACK:unpowered " + e.name);
      return false;
    }
    trace_.push_back("probe 0x" + hex(address) + " ACK " + e.name);
    return true;
  }

  bool write(uint8_t address, const std::vector<uint8_t>& bytes, bool firmwareView = true) {
    if (!probe(address, firmwareView)) return false;
    auto& e = entries_.at(address);
    const bool ok = e.device && e.device->write(bytes);
    trace_.push_back(std::string("write ") + e.name + (ok ? " ACK" : " NACK:device"));
    return ok;
  }

  bool read(uint8_t address, size_t count, std::vector<uint8_t>& out,
            bool firmwareView = true) {
    if (!probe(address, firmwareView)) return false;
    auto& e = entries_.at(address);
    const bool ok = e.device && e.device->read(count, out);
    trace_.push_back(std::string("read ") + e.name + (ok ? " ACK" : " NACK:device"));
    return ok;
  }

  int sda() const { return sda_; }
  int scl() const { return scl_; }
  uint32_t hz() const { return hz_; }
  const std::vector<std::string>& trace() const { return trace_; }
  const std::vector<std::string>& errors() const { return errors_; }

 private:
  static std::string hex(uint8_t v) {
    constexpr char k[] = "0123456789abcdef";
    std::string s;
    s += k[(v >> 4) & 0xF];
    s += k[v & 0xF];
    return s;
  }

  int sda_;
  int scl_;
  uint32_t hz_;
  std::map<uint8_t, Entry> entries_;
  std::vector<std::string> trace_;
  std::vector<std::string> errors_;
};

}  // namespace m4sim

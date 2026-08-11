#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace m4sim {

class SimSpiSink {
 public:
  virtual ~SimSpiSink() = default;
  virtual void selected() {}
  virtual void deselected() {}
  virtual bool transfer(bool dataMode, const uint8_t* bytes, size_t count) = 0;
};

class SimSpiBus {
 public:
  struct Device {
    std::string name;
    int cs = -1;
    uint32_t maxHz = 0;
    SimSpiSink* sink = nullptr;
  };

  explicit SimSpiBus(int sclk, int mosi, int miso = -1)
      : sclk_(sclk), mosi_(mosi), miso_(miso) {}

  bool attach(int cs, std::string name, SimSpiSink* sink, uint32_t maxHz = 0) {
    if (devices_.count(cs)) return fail("SPI CS collision GPIO" + std::to_string(cs));
    devices_.emplace(cs, Device{std::move(name), cs, maxHz, sink});
    return true;
  }

  bool begin(int cs, uint32_t hz) {
    if (active_) return fail("nested SPI transaction");
    auto it = devices_.find(cs);
    if (it == devices_.end()) return fail("SPI transaction to unattached CS GPIO" + std::to_string(cs));
    if (it->second.maxHz && hz > it->second.maxHz) {
      return fail(it->second.name + " SPI clock " + std::to_string(hz) +
                  " exceeds modeled max " + std::to_string(it->second.maxHz));
    }
    active_ = &it->second;
    activeHz_ = hz;
    transferred_ = 0;
    if (active_->sink) active_->sink->selected();
    trace_.push_back("BEGIN " + active_->name + " hz=" + std::to_string(hz));
    return true;
  }

  bool writeCommand(uint8_t command) { return transfer(false, &command, 1); }

  bool writeData(const uint8_t* bytes, size_t count) { return transfer(true, bytes, count); }
  bool writeData(const std::vector<uint8_t>& bytes) { return writeData(bytes.data(), bytes.size()); }
  bool writeData(uint8_t value) { return writeData(&value, 1); }

  bool end() {
    if (!active_) return fail("SPI end without begin");
    trace_.push_back("END " + active_->name + " bytes=" + std::to_string(transferred_));
    if (active_->sink) active_->sink->deselected();
    active_ = nullptr;
    activeHz_ = 0;
    transferred_ = 0;
    return true;
  }

  bool active() const { return active_ != nullptr; }
  uint32_t activeHz() const { return activeHz_; }
  const std::vector<std::string>& trace() const { return trace_; }
  const std::vector<std::string>& errors() const { return errors_; }
  bool ok() const { return errors_.empty(); }

  int sclk() const { return sclk_; }
  int mosi() const { return mosi_; }
  int miso() const { return miso_; }

 private:
  bool transfer(bool dataMode, const uint8_t* bytes, size_t count) {
    if (!active_) return fail("SPI transfer without active transaction");
    if (!bytes && count) return fail("SPI transfer null buffer");
    transferred_ += count;
    if (!active_->sink) return true;
    const bool ok = active_->sink->transfer(dataMode, bytes, count);
    if (!ok) fail(active_->name + " rejected SPI transfer");
    return ok;
  }

  bool fail(const std::string& msg) {
    errors_.push_back(msg);
    return false;
  }

  int sclk_;
  int mosi_;
  int miso_;
  std::map<int, Device> devices_;
  Device* active_ = nullptr;
  uint32_t activeHz_ = 0;
  size_t transferred_ = 0;
  std::vector<std::string> trace_;
  std::vector<std::string> errors_;
};

}  // namespace m4sim

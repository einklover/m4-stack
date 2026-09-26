#pragma once
#include <cstdint>
#include "M4HttpDownloadPolicy.h"

// HTTPClient can spin on available()==0 for unknown-length bodies without
// writing to the sink. Enforce deadlines at the socket boundary, not the sink.
template <typename Client>
class M4DeadlineClient final : public Client {
 public:
  void startBody(uint32_t totalMs) {
    started_ = progressed_ = millis();
    totalMs_ = totalMs;
    armed_ = true;
  }
  bool expired() const { return expired_; }
  int available() override { return check() ? Client::available() : 0; }
  uint8_t connected() override { return check() ? Client::connected() : 0; }
  int read(uint8_t* data, size_t n) override {
    if (!check()) return -1;
    const int got = Client::read(data, n);
    if (got > 0) progressed_ = millis();
    return got;
  }
  int read() override {
    uint8_t byte;
    return read(&byte, 1) == 1 ? byte : -1;
  }
 private:
  bool check() {
    if (expired_) return false;
    const uint32_t now = millis();
    if (armed_ && (now - started_ >= totalMs_ ||
                   now - progressed_ >= M4HttpDownloadPolicy::kIdleTimeoutMs)) {
      expired_ = true;
      Client::stop();
      return false;
    }
    return true;
  }
  uint32_t started_ = 0, progressed_ = 0, totalMs_ = 0;
  bool armed_ = false, expired_ = false;
};

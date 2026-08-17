#pragma once

#include <cstddef>
#include <cstdint>

namespace M4xProgressiveHttpState {

// Wrap-safe inactivity window for decoded HTTP payload. Transport/framing
// bytes deliberately do not renew this window; only accepted body payload
// should call onPayload().
class PayloadInactivityWindow {
 public:
  void reset(uint32_t nowMs) { lastProgressMs_ = nowMs; }
  void onPayload(uint32_t nowMs) { lastProgressMs_ = nowMs; }

  bool expired(uint32_t nowMs, uint32_t timeoutMs) const {
    return static_cast<uint32_t>(nowMs - lastProgressMs_) >= timeoutMs;
  }

  uint32_t lastProgressMs() const { return lastProgressMs_; }

 private:
  uint32_t lastProgressMs_ = 0;
};

// Stateful CRLF accumulator for chunk boundaries. The two framing bytes may
// arrive in different pump()/readDecoded() calls, so they must outlive a
// single stack frame.
class ChunkCrlfAccumulator {
 public:
  void reset() { used_ = 0; }
  uint8_t* writePtr() { return bytes_ + used_; }
  size_t remaining() const { return sizeof(bytes_) - used_; }
  void commit(size_t n) {
    const size_t room = remaining();
    used_ += n > room ? room : n;
  }
  bool complete() const { return used_ == sizeof(bytes_); }
  bool valid() const { return complete() && bytes_[0] == '\r' && bytes_[1] == '\n'; }
  size_t used() const { return used_; }

 private:
  uint8_t bytes_[2] = {0, 0};
  size_t used_ = 0;
};

}  // namespace M4xProgressiveHttpState

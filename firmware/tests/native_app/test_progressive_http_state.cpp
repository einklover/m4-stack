#include "apps/M4xProgressiveHttpState.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

void long_lived_payload_stream() {
  constexpr uint32_t kTimeoutMs = 45000;
  M4xProgressiveHttpState::PayloadInactivityWindow window;
  window.reset(0);

  // The response lives for 80 s (>45 s), but decoded payload advances every
  // 10 s, so lifetime alone must never trip the inactivity timeout.
  for (uint32_t now = 10000; now <= 80000; now += 10000) {
    assert(!window.expired(now, kTimeoutMs));
    window.onPayload(now);
  }
  assert(window.lastProgressMs() == 80000);
}

void real_payload_silence_times_out() {
  constexpr uint32_t kTimeoutMs = 45000;
  M4xProgressiveHttpState::PayloadInactivityWindow window;
  window.reset(10000);
  assert(!window.expired(54999, kTimeoutMs));
  assert(window.expired(55000, kTimeoutMs));
}

void framing_noise_does_not_renew_payload_deadline() {
  constexpr uint32_t kTimeoutMs = 45000;
  M4xProgressiveHttpState::PayloadInactivityWindow window;
  window.reset(1000);

  // Transport/chunk framing activity is deliberately not reported via
  // onPayload(). It must not keep a response alive forever without body data.
  for (uint32_t transportNow : {10000u, 20000u, 30000u, 40000u}) {
    assert(!window.expired(transportNow, kTimeoutMs));
  }
  assert(window.expired(46000, kTimeoutMs));
}

void millis_wrap_is_safe() {
  constexpr uint32_t kTimeoutMs = 100;
  M4xProgressiveHttpState::PayloadInactivityWindow window;
  window.reset(std::numeric_limits<uint32_t>::max() - 50u);
  assert(!window.expired(20u, kTimeoutMs));  // 71 ms elapsed across wrap.
  assert(window.expired(49u, kTimeoutMs));  // 100 ms elapsed across wrap.
}

void fragmented_chunk_crlf_survives_pump_boundary() {
  M4xProgressiveHttpState::ChunkCrlfAccumulator crlf;
  crlf.reset();

  *crlf.writePtr() = '\r';
  crlf.commit(1);
  assert(crlf.used() == 1);
  assert(!crlf.complete());

  // Simulate returning from one pump/readDecoded call here. The CR byte must
  // remain stateful so the next call consumes only the missing LF.
  *crlf.writePtr() = '\n';
  crlf.commit(1);
  assert(crlf.complete());
  assert(crlf.valid());

  crlf.reset();
  *crlf.writePtr() = '\n';
  crlf.commit(1);
  *crlf.writePtr() = '4';
  crlf.commit(1);
  assert(crlf.complete());
  assert(!crlf.valid());
}

}  // namespace

int main() {
  long_lived_payload_stream();
  real_payload_silence_times_out();
  framing_noise_does_not_renew_payload_deadline();
  millis_wrap_is_safe();
  fragmented_chunk_crlf_survives_pump_boundary();
  std::cout << "progressive HTTP inactivity/chunk framing tests passed\n";
  return 0;
}

// Host tests for M4B3 Browser Bridge hardware-key codec and bounded queue.
// Build: g++ -std=c++14 -Wall -Wextra -Werror -I firmware/src \
//        firmware/tests/native_app/test_m4b3_key.cpp -o /tmp/test_m4b3_key

#include <cassert>
#include <cstdint>
#include <cstdio>

#include "util/M4B3Key.h"
#include "util/M4B3Protocol.h"

int main() {
  {
    uint8_t wire[64] = {};
    const size_t n = M4B3::encodeInputKey(wire, sizeof(wire), 21, M4B3::kInputKeyBack, 0, 1234, 7, 3);
    assert(n == M4B3::kEnvelopeSize + M4B3::kInputKeyHeaderSize);
    assert(wire[4] == M4B3::kTypeInputKey);
    M4B3::Envelope env;
    size_t need = 0;
    assert(M4B3::parseEnvelope(wire, n, env, &need) == M4B3::Status::Ok);
    assert(need == n);
    assert(env.type == M4B3::kTypeInputKey);
    assert(env.headerLen == M4B3::kInputKeyHeaderSize);
    assert(env.payloadLen == 0);

    M4B3Key::Event ev;
    assert(M4B3Key::parseInputKey(wire + M4B3::kEnvelopeSize, env.headerLen, env.payloadLen, ev) ==
           M4B3::Status::Ok);
    assert(ev.action == M4B3::kInputKeyBack);
    assert(ev.tMs == 1234);
    assert(ev.seq == 7);
    assert(ev.session == 3);

    assert(M4B3Key::parseInputKey(nullptr, M4B3::kInputKeyHeaderSize, 0, ev) == M4B3::Status::Invalid);
    assert(M4B3Key::parseInputKey(wire + M4B3::kEnvelopeSize, 8, 0, ev) == M4B3::Status::Invalid);
    assert(M4B3Key::parseInputKey(wire + M4B3::kEnvelopeSize, M4B3::kInputKeyHeaderSize, 1, ev) ==
           M4B3::Status::Invalid);

    uint8_t bad[M4B3::kInputKeyHeaderSize] = {};
    assert(M4B3Key::parseInputKey(bad, sizeof(bad), 0, ev) == M4B3::Status::Invalid);
    bad[0] = 3;
    assert(M4B3Key::parseInputKey(bad, sizeof(bad), 0, ev) == M4B3::Status::Invalid);
  }

  {
    M4B3Key::Queue q;
    assert(q.session() == 1);
    assert(q.push(M4B3::kInputKeyBack, 100) == M4B3Key::PushResult::Ok);
    assert(q.push(M4B3::kInputKeyReload, 101) == M4B3Key::PushResult::Ok);
    assert(q.push(0, 102) == M4B3Key::PushResult::Rejected);
    assert(q.size() == 2);
    M4B3Key::Event a;
    M4B3Key::Event b;
    assert(q.pop(a));
    assert(q.pop(b));
    assert(!q.pop(b));
    assert(a.action == M4B3::kInputKeyBack && a.seq == 0 && a.session == 1);
    assert(b.action == M4B3::kInputKeyReload && b.seq == 1 && b.session == 1);
    assert(q.stats().back == 1);
    assert(q.stats().reload == 1);
    assert(q.stats().rejected == 1);
  }

  {
    M4B3Key::Queue q;
    for (size_t i = 0; i < M4B3Key::kQueueDepth; ++i) {
      assert(q.push((i & 1) ? M4B3::kInputKeyReload : M4B3::kInputKeyBack,
                    static_cast<uint32_t>(i)) == M4B3Key::PushResult::Ok);
    }
    assert(q.size() == M4B3Key::kQueueDepth);
    assert(q.push(M4B3::kInputKeyBack, 99) == M4B3Key::PushResult::Overflow);
    assert(q.stats().overflow == 1);
    q.resetSession();
    assert(q.empty());
    assert(q.session() == 2);
    assert(q.stats().sessionResets == 1);
    assert(q.push(M4B3::kInputKeyBack, 100) == M4B3Key::PushResult::Ok);
    M4B3Key::Event ev;
    assert(q.pop(ev));
    assert(ev.seq == 0);
    assert(ev.session == 2);
  }

  // Phone must never send INPUT_KEY back into the firmware session. It is
  // accepted by the stream parser but ignored without mutating framebuffer or
  // counting a protocol apply error, matching the existing TOUCH direction.
  {
    uint8_t accepted[M4B3::kKeyframeSize];
    uint8_t candidate[M4B3::kKeyframeSize];
    M4B3::Session session;
    session.attach(accepted, candidate);
    uint8_t wire[64] = {};
    const size_t n = M4B3::encodeInputKey(wire, sizeof(wire), 1, M4B3::kInputKeyBack, 0, 10, 0, 1);
    uint8_t reply[32] = {};
    const uint32_t errBefore = session.stats().applyErrors;
    assert(session.handle(wire, n, reply, sizeof(reply)) == 0);
    assert(session.stats().applyErrors == errBefore);
    assert(session.acceptedFrameId() < 0);
  }

  std::printf("OK: M4B3 hardware key codec/queue\n");
  return 0;
}

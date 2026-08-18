// Host tests for M4B3 TOUCH mapping, codec, and bounded single-pointer queue.
// Build: g++ -std=c++14 -Wall -Wextra -Werror -I firmware/src
//        firmware/tests/native_app/test_m4b3_input.cpp -o /tmp/test_m4b3_input

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "util/M4B3Input.h"
#include "util/M4B3Protocol.h"
#include "util/M4PanelMapper.h"

namespace {

void expectLogical(int phyX, int phyY, int wantX, int wantY) {
  int lx = -1;
  int ly = -1;
  assert(M4B3Input::panelToLogical(phyX, phyY, &lx, &ly));
  assert(lx == wantX);
  assert(ly == wantY);
}

void expectRoundTrip(int logicalX, int logicalY) {
  int px = 0;
  int py = 0;
  M4PanelMapper::logicalToPhysical(logicalX, logicalY, &px, &py);
  int lx = -1;
  int ly = -1;
  assert(M4B3Input::panelToLogical(px, py, &lx, &ly));
  assert(lx == logicalX);
  assert(ly == logicalY);
}

void drain(M4B3Input::Queue& q, std::vector<M4B3Input::Event>& out) {
  M4B3Input::Event ev;
  while (q.pop(ev)) out.push_back(ev);
}

}  // namespace

int main() {
  // Inverse of GfxRenderer Portrait / M4PanelMapper: logicalX=479-phyY, logicalY=phyX.
  expectLogical(0, 479, 0, 0);        // TL
  expectLogical(0, 0, 479, 0);        // TR
  expectLogical(799, 479, 0, 799);    // BL
  expectLogical(799, 0, 479, 799);    // BR
  expectLogical(400, 239, 240, 400);  // center-ish
  expectLogical(140, 399, 80, 140);   // asymmetric A
  expectLogical(456, 159, 320, 456);  // asymmetric B
  expectLogical(0, 240, 239, 0);
  expectLogical(399, 0, 479, 399);

  expectRoundTrip(0, 0);
  expectRoundTrip(479, 0);
  expectRoundTrip(0, 799);
  expectRoundTrip(479, 799);
  expectRoundTrip(240, 400);
  expectRoundTrip(80, 140);
  expectRoundTrip(320, 456);
  expectRoundTrip(1, 1);
  expectRoundTrip(478, 798);

  {
    int lx = 0;
    int ly = 0;
    assert(!M4B3Input::panelToLogical(-1, 0, &lx, &ly));
    assert(!M4B3Input::panelToLogical(800, 0, &lx, &ly));
    assert(!M4B3Input::panelToLogical(0, -1, &lx, &ly));
    assert(!M4B3Input::panelToLogical(0, 480, &lx, &ly));
    assert(!M4B3Input::panelToLogical(0, 0, nullptr, &ly));
  }

  // Codec roundtrip + malformed/truncated/oversize/out-of-range.
  {
    uint8_t wire[64];
    const size_t n = M4B3::encodeTouch(wire, sizeof(wire), 11, M4B3::kTouchDown, 0, 80, 140, 1234, 7, 3);
    assert(n == M4B3::kEnvelopeSize + M4B3::kTouchHeaderSize);
    assert(wire[4] == M4B3::kTypeTouch);
    M4B3::Envelope env;
    size_t need = 0;
    assert(M4B3::parseEnvelope(wire, n, env, &need) == M4B3::Status::Ok);
    assert(env.type == M4B3::kTypeTouch);
    assert(env.headerLen == M4B3::kTouchHeaderSize);
    assert(env.payloadLen == 0);
    M4B3Input::Event ev;
    assert(M4B3Input::parseTouch(wire + M4B3::kEnvelopeSize, env.headerLen, env.payloadLen, ev) ==
           M4B3::Status::Ok);
    assert(ev.action == M4B3::kTouchDown);
    assert(ev.x == 80);
    assert(ev.y == 140);
    assert(ev.tMs == 1234);
    assert(ev.seq == 7);
    assert(ev.session == 3);

    assert(M4B3Input::parseTouch(wire + M4B3::kEnvelopeSize, 10, 0, ev) == M4B3::Status::Invalid);
    assert(M4B3Input::parseTouch(wire + M4B3::kEnvelopeSize, M4B3::kTouchHeaderSize, 4, ev) ==
           M4B3::Status::Invalid);
    assert(M4B3Input::parseTouch(nullptr, M4B3::kTouchHeaderSize, 0, ev) == M4B3::Status::Invalid);

    uint8_t badAct[M4B3::kTouchHeaderSize] = {};
    assert(M4B3Input::parseTouch(badAct, M4B3::kTouchHeaderSize, 0, ev) == M4B3::Status::Invalid);
    badAct[0] = 5;
    assert(M4B3Input::parseTouch(badAct, M4B3::kTouchHeaderSize, 0, ev) == M4B3::Status::Invalid);

    uint8_t oor[M4B3::kTouchHeaderSize] = {};
    oor[0] = M4B3::kTouchMove;
    M4B3::wr16(oor + 4, 480);
    M4B3::wr16(oor + 6, 0);
    assert(M4B3Input::parseTouch(oor, M4B3::kTouchHeaderSize, 0, ev) == M4B3::Status::Invalid);
    M4B3::wr16(oor + 4, 0);
    M4B3::wr16(oor + 6, 800);
    assert(M4B3Input::parseTouch(oor, M4B3::kTouchHeaderSize, 0, ev) == M4B3::Status::Invalid);

    uint8_t tiny[8] = {'M', '4', 'B', '3', M4B3::kTypeTouch, 0, 20, 0};
    assert(M4B3::parseEnvelope(tiny, sizeof(tiny), env, &need) != M4B3::Status::Ok);
  }

  // Session.handle ignores inbound TOUCH (phone should never send it).
  {
    std::vector<uint8_t> accepted(M4B3::kKeyframeSize, 0xFF);
    std::vector<uint8_t> candidate(M4B3::kKeyframeSize, 0xFF);
    M4B3::Session session;
    session.attach(accepted.data(), candidate.data());
    uint8_t wire[64];
    const size_t n = M4B3::encodeTouch(wire, sizeof(wire), 1, M4B3::kTouchDown, 0, 10, 10, 0, 0, 1);
    uint8_t reply[32];
    const uint32_t errBefore = session.stats().applyErrors;
    assert(session.handle(wire, n, reply, sizeof(reply)) == 0);
    assert(session.stats().applyErrors == errBefore);
    assert(session.acceptedFrameId() < 0);
  }

  // Queue: DOWN/UP lossless, MOVE latest-wins, bounded, reconnect reset.
  {
    M4B3Input::Queue q;
    assert(q.session() == 1);
    assert(q.push(M4B3::kTouchDown, 10, 20, 100) == M4B3Input::PushResult::Ok);
    assert(q.push(0, 10, 20, 101) == M4B3Input::PushResult::Rejected);
    assert(q.push(M4B3::kTouchMove, 480, 0, 102) == M4B3Input::PushResult::Rejected);
    assert(q.push(M4B3::kTouchMove, 11, 21, 103) == M4B3Input::PushResult::Ok);
    assert(q.push(M4B3::kTouchMove, 12, 22, 104) == M4B3Input::PushResult::Coalesced);
    assert(q.push(M4B3::kTouchMove, 13, 23, 105) == M4B3Input::PushResult::Coalesced);
    assert(q.size() == 2);
    assert(q.push(M4B3::kTouchUp, 13, 23, 106) == M4B3Input::PushResult::Ok);
    assert(q.size() == 3);
    std::vector<M4B3Input::Event> evs;
    drain(q, evs);
    assert(evs.size() == 3);
    assert(evs[0].action == M4B3::kTouchDown && evs[0].x == 10 && evs[0].y == 20);
    assert(evs[1].action == M4B3::kTouchMove && evs[1].x == 13 && evs[1].y == 23);
    assert(evs[2].action == M4B3::kTouchUp);
    assert(q.stats().coalesced == 2);
    assert(q.stats().down == 1);
    assert(q.stats().up == 1);
    assert(!q.active());
  }

  {
    M4B3Input::Queue q;
    assert(q.push(M4B3::kTouchMove, 1, 1, 1) == M4B3Input::PushResult::Rejected);
    assert(q.push(M4B3::kTouchUp, 1, 1, 1) == M4B3Input::PushResult::Rejected);
    assert(q.push(M4B3::kTouchDown, 5, 6, 10) == M4B3Input::PushResult::Ok);
    assert(q.push(M4B3::kTouchDown, 7, 8, 11) == M4B3Input::PushResult::Ok);
    std::vector<M4B3Input::Event> evs;
    drain(q, evs);
    assert(evs.size() == 3);
    assert(evs[0].action == M4B3::kTouchDown);
    assert(evs[1].action == M4B3::kTouchCancel);
    assert(evs[2].action == M4B3::kTouchDown && evs[2].x == 7);
    assert(q.stats().implicitCancel == 1);
    assert(q.active());
  }

  {
    M4B3Input::Queue q;
    assert(q.push(M4B3::kTouchDown, 1, 1, 1) == M4B3Input::PushResult::Ok);
    q.resetSession();
    assert(!q.active());
    assert(q.empty());
    assert(q.session() == 2);
    assert(q.stats().sessionResets == 1);
    assert(q.stats().sessionCancels == 1);
    assert(q.push(M4B3::kTouchMove, 2, 2, 2) == M4B3Input::PushResult::Rejected);
    assert(q.push(M4B3::kTouchDown, 3, 4, 3) == M4B3Input::PushResult::Ok);
    M4B3Input::Event ev;
    assert(q.pop(ev));
    assert(ev.session == 2);
    assert(ev.action == M4B3::kTouchDown);
  }

  {
    M4B3Input::Queue q;
    // Stacked DOWNs fill lossless slots: 1 + 2*(n-1). Fourth DOWN => 7 events.
    assert(q.push(M4B3::kTouchDown, 1, 1, 1) == M4B3Input::PushResult::Ok);
    for (int i = 0; i < 3; ++i) {
      assert(q.push(M4B3::kTouchDown, static_cast<uint16_t>(2 + i), 1, static_cast<uint32_t>(2 + i)) ==
             M4B3Input::PushResult::Ok);
    }
    assert(q.size() == 7);
    const M4B3Input::PushResult fifth =
        q.push(M4B3::kTouchDown, 9, 1, 9);
    assert(fifth == M4B3Input::PushResult::Ok || fifth == M4B3Input::PushResult::Overflow);
    assert(q.size() <= M4B3Input::kQueueDepth);
    assert(q.stats().down + q.stats().cancel <= 16);
  }

  {
    M4B3Input::Queue q;
    assert(q.push(M4B3::kTouchDown, 10, 10, 1) == M4B3Input::PushResult::Ok);
    for (int i = 0; i < 20; ++i) {
      const M4B3Input::PushResult st =
          q.push(M4B3::kTouchMove, static_cast<uint16_t>(10 + i), 10, static_cast<uint32_t>(2 + i));
      assert(st == M4B3Input::PushResult::Ok || st == M4B3Input::PushResult::Coalesced);
    }
    assert(q.size() == 2);
    assert(q.push(M4B3::kTouchCancel, 29, 10, 30) == M4B3Input::PushResult::Ok);
    std::vector<M4B3Input::Event> evs;
    drain(q, evs);
    assert(evs.size() == 3);
    assert(evs[0].action == M4B3::kTouchDown);
    assert(evs[1].action == M4B3::kTouchMove);
    assert(evs[2].action == M4B3::kTouchCancel);
    assert(!q.active());
  }

  // Bounded MOVE coalescing stays depth=2 across a long latest-wins stream.
  {
    M4B3Input::Queue q;
    assert(q.push(M4B3::kTouchDown, 10, 10, 1) == M4B3Input::PushResult::Ok);
    uint32_t coalesced = 0;
    for (int i = 0; i < 10000; ++i) {
      const M4B3Input::PushResult st =
          q.push(M4B3::kTouchMove, static_cast<uint16_t>((10 + i) % 480),
                 static_cast<uint16_t>((10 + (i / 3)) % 800), static_cast<uint32_t>(2 + i));
      assert(st == M4B3Input::PushResult::Ok || st == M4B3Input::PushResult::Coalesced);
      if (st == M4B3Input::PushResult::Coalesced) ++coalesced;
      assert(q.size() <= 2);
    }
    assert(q.size() == 2);
    assert(coalesced >= 9999u);
    assert(q.push(M4B3::kTouchUp, 20, 20, 20000) == M4B3Input::PushResult::Ok);
    assert(q.size() == 3);
    std::vector<M4B3Input::Event> evs;
    drain(q, evs);
    assert(evs.size() == 3);
    assert(evs[0].action == M4B3::kTouchDown);
    assert(evs[1].action == M4B3::kTouchMove);
    assert(evs[1].x == static_cast<uint16_t>((10 + 9999) % 480));
    assert(evs[2].action == M4B3::kTouchUp);
    assert(!q.active());
    assert(q.stats().coalesced >= 9999u);
  }

  std::printf("OK: M4B3 input mapping/codec/queue\n");
  return 0;
}

// Host tests for M4B3 firmware parser + atomic session.
// Build: g++-14 -std=c++14 -I firmware/src firmware/tests/native_app/test_m4b3_receiver.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "util/M4B3Protocol.h"

namespace {

std::vector<uint8_t> white() {
  return std::vector<uint8_t>(M4B3::kKeyframeSize, 0xFF);
}

void setBlack(std::vector<uint8_t>& fb, int x, int y) {
  const size_t off = static_cast<size_t>(y) * M4B3::kStride + static_cast<size_t>(x >> 3);
  fb[off] = static_cast<uint8_t>(fb[off] & ~(0x80u >> (x & 7)));
}

size_t encodeOneRectPatch(uint8_t* out, size_t cap, uint32_t seq, uint32_t frameId, uint32_t baseId,
                          const std::vector<uint8_t>& finalFb, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  const uint16_t stride = static_cast<uint16_t>(w >> 3);
  std::vector<uint8_t> payload(static_cast<size_t>(M4B3::kRectMetaSize) + static_cast<size_t>(stride) * h);
  M4B3::wr16(payload.data(), x);
  M4B3::wr16(payload.data() + 2, y);
  M4B3::wr16(payload.data() + 4, w);
  M4B3::wr16(payload.data() + 6, h);
  M4B3::wr32(payload.data() + 8, static_cast<uint32_t>(stride) * h);
  for (uint16_t row = 0; row < h; ++row) {
    std::memcpy(payload.data() + M4B3::kRectMetaSize + static_cast<size_t>(row) * stride,
                finalFb.data() + static_cast<size_t>(y + row) * M4B3::kStride + (x >> 3), stride);
  }
  uint8_t header[M4B3::kPatchHeaderSize];
  M4B3::wr32(header, frameId);
  M4B3::wr32(header + 4, baseId);
  M4B3::wr16(header + 8, 1);
  M4B3::wr32(header + 10, M4B3::crc32(finalFb.data(), finalFb.size()));
  return M4B3::wrap(out, cap, M4B3::kTypeFramePatch, 0, seq, header, M4B3::kPatchHeaderSize, payload.data(),
                    static_cast<uint32_t>(payload.size()));
}

struct Harness {
  std::vector<uint8_t> accepted;
  std::vector<uint8_t> candidate;
  M4B3::Session session;
  uint8_t reply[80]{};

  Harness() : accepted(M4B3::kKeyframeSize, 0xFF), candidate(M4B3::kKeyframeSize, 0xFF) {
    session.attach(accepted.data(), candidate.data());
  }

  size_t handle(const uint8_t* msg, size_t len) { return session.handle(msg, len, reply, sizeof(reply)); }
};

void expectType(const uint8_t* msg, size_t n, uint8_t type) {
  assert(n >= M4B3::kEnvelopeSize);
  M4B3::Envelope env;
  size_t need = 0;
  assert(M4B3::parseEnvelope(msg, n, env, &need) == M4B3::Status::Ok);
  assert(env.type == type);
}

}  // namespace

int main() {
  {
    const char check[] = "123456789";
    assert(M4B3::crc32(reinterpret_cast<const uint8_t*>(check), 9) == M4B3::kCrc32Check);
  }

  uint8_t wire[M4B3::kMaxMessageSize];

  // HELLO ok + version reject
  {
    Harness h;
    const size_t n = M4B3::encodeHello(wire, sizeof(wire), 1, M4B3::kHelloOk);
    const size_t r = h.handle(wire, n);
    expectType(h.reply, r, M4B3::kTypeHello);
    assert(h.session.helloOk());
    assert(h.reply[M4B3::kEnvelopeSize + 16] == M4B3::kHelloOk);
  }
  {
    Harness h;
    uint8_t header[M4B3::kHelloHeaderSize];
    header[0] = 2;
    M4B3::wr16(header + 1, M4B3::kWidth);
    M4B3::wr16(header + 3, M4B3::kHeight);
    header[5] = M4B3::kPixelMono1;
    M4B3::wr16(header + 6, M4B3::kStride);
    M4B3::wr32(header + 8, M4B3::kV1Capabilities);
    M4B3::wr32(header + 12, M4B3::kMaxPayloadLen);
    header[16] = M4B3::kHelloOk;
    const size_t n = M4B3::wrap(wire, sizeof(wire), M4B3::kTypeHello, 0, 1, header, M4B3::kHelloHeaderSize, nullptr, 0);
    const size_t r = h.handle(wire, n);
    assert(h.reply[M4B3::kEnvelopeSize + 16] == M4B3::kHelloUnsupportedVersion);
    assert(!h.session.helloOk());
    (void)r;
  }

  // keyframe + sparse patch, CRC equal
  Harness h;
  auto fb = white();
  const size_t helloN = M4B3::encodeHello(wire, sizeof(wire), 0, M4B3::kHelloOk);
  h.handle(wire, helloN);
  const size_t keyN = M4B3::encodeKeyframe(wire, sizeof(wire), 1, 0, fb.data());
  size_t r = h.handle(wire, keyN);
  expectType(h.reply, r, M4B3::kTypeFrameAck);
  assert(h.reply[M4B3::kEnvelopeSize + 4] == M4B3::kAckOk);
  assert(h.session.acceptedFrameId() == 0);
  assert(h.session.acceptedCrc() == M4B3::crc32(fb.data(), fb.size()));
  assert(h.session.stats().keys == 1);

  auto next = fb;
  for (int x = 0; x < 16; ++x)
    for (int y = 0; y < 16; ++y) setBlack(next, x, y);
  const size_t patchN = encodeOneRectPatch(wire, sizeof(wire), 2, 1, 0, next, 0, 0, 16, 16);
  r = h.handle(wire, patchN);
  assert(h.reply[M4B3::kEnvelopeSize + 4] == M4B3::kAckOk);
  assert(h.session.acceptedFrameId() == 1);
  assert(h.session.acceptedCrc() == M4B3::crc32(next.data(), next.size()));
  assert(h.session.stats().patches == 1);
  assert(std::memcmp(h.session.accepted(), next.data(), M4B3::kKeyframeSize) == 0);

  // wrong base does not mutate accepted
  {
    const int32_t beforeId = h.session.acceptedFrameId();
    const uint32_t beforeCrc = h.session.acceptedCrc();
    auto rogue = next;
    setBlack(rogue, 32, 32);
    const size_t n = encodeOneRectPatch(wire, sizeof(wire), 3, 9, 99, rogue, 32, 32, 16, 16);
    r = h.handle(wire, n);
    assert(h.reply[M4B3::kEnvelopeSize + 4] == M4B3::kNackBase);
    assert(h.session.acceptedFrameId() == beforeId);
    assert(h.session.acceptedCrc() == beforeCrc);
    assert(std::memcmp(h.session.accepted(), next.data(), M4B3::kKeyframeSize) == 0);
  }

  // CRC mismatch does not mutate accepted
  {
    const int32_t beforeId = h.session.acceptedFrameId();
    const uint32_t beforeCrc = h.session.acceptedCrc();
    auto flipped = next;
    setBlack(flipped, 48, 48);
    const size_t n = encodeOneRectPatch(wire, sizeof(wire), 4, 2, 1, flipped, 48, 48, 16, 16);
    wire[M4B3::kEnvelopeSize + 10] ^= 0xFF;  // corrupt claimed CRC
    r = h.handle(wire, n);
    assert(h.reply[M4B3::kEnvelopeSize + 4] == M4B3::kNackCrc);
    assert(h.session.acceptedFrameId() == beforeId);
    assert(h.session.acceptedCrc() == beforeCrc);
  }

  // oversized payload rejected before treating as a session body
  {
    uint8_t tiny[32];
    std::memset(tiny, 0, sizeof(tiny));
    std::memcpy(tiny, M4B3::kMagic, 4);
    tiny[4] = M4B3::kTypeFrameKey;
    M4B3::wr16(tiny + 6, 0);
    M4B3::wr32(tiny + 8, M4B3::kMaxPayloadLen + 1);
    M4B3::Envelope env;
    size_t need = 0;
    assert(M4B3::parseEnvelope(tiny, sizeof(tiny), env, &need) == M4B3::Status::Oversized);
  }

  // invalid rectangle (not byte aligned)
  {
    const int32_t beforeId = h.session.acceptedFrameId();
    uint8_t header[M4B3::kPatchHeaderSize];
    M4B3::wr32(header, 3);
    M4B3::wr32(header + 4, 1);
    M4B3::wr16(header + 8, 1);
    M4B3::wr32(header + 10, 0);
    uint8_t rect[M4B3::kRectMetaSize] = {};
    M4B3::wr16(rect, 1);  // x=1 not aligned
    M4B3::wr16(rect + 2, 0);
    M4B3::wr16(rect + 4, 16);
    M4B3::wr16(rect + 6, 16);
    M4B3::wr32(rect + 8, 32);
    const size_t n = M4B3::wrap(wire, sizeof(wire), M4B3::kTypeFramePatch, 0, 5, header, M4B3::kPatchHeaderSize, rect,
                                sizeof(rect));
    r = h.handle(wire, n);
    assert(h.reply[M4B3::kEnvelopeSize + 4] == M4B3::kNackMalformed);
    assert(h.session.acceptedFrameId() == beforeId);
  }

  // fragmented + coalesced stream
  {
    Harness hs;
    std::vector<uint8_t> rx(M4B3::kMaxMessageSize);
    M4B3::StreamParser p;
    p.attach(rx.data(), rx.size());
    uint8_t hello[64];
    const size_t hn = M4B3::encodeHello(hello, sizeof(hello), 1, M4B3::kHelloOk);
    uint8_t ping[32];
    const size_t pn = M4B3::encodePingPong(ping, sizeof(ping), M4B3::kTypePing, 2, 0xAABBCCDDu);
    std::vector<uint8_t> both;
    both.insert(both.end(), hello, hello + hn);
    both.insert(both.end(), ping, ping + pn);

    size_t off = 0;
    int msgs = 0;
    while (off < both.size()) {
      const size_t chunk = (off % 5) + 1;
      const size_t n = off + chunk > both.size() ? both.size() - off : chunk;
      assert(p.append(both.data() + off, n) == M4B3::Status::Ok);
      off += n;
      const uint8_t* msg = nullptr;
      size_t msgLen = 0;
      while (p.nextMessage(&msg, &msgLen) == M4B3::Status::Ok) {
        hs.handle(msg, msgLen);
        p.consume(msgLen);
        msgs++;
      }
    }
    assert(msgs == 2);
    assert(hs.session.helloOk());
    assert(hs.session.stats().pings == 1);
    assert(hs.reply[4] == M4B3::kTypePong);
    assert(M4B3::rd32(hs.reply + M4B3::kEnvelopeSize) == 0xAABBCCDDu);
  }

  // disconnect mid-message: leftover discarded, accepted unchanged
  {
    Harness hs;
    auto base = white();
    size_t n = M4B3::encodeHello(wire, sizeof(wire), 0, M4B3::kHelloOk);
    hs.handle(wire, n);
    n = M4B3::encodeKeyframe(wire, sizeof(wire), 1, 0, base.data());
    hs.handle(wire, n);
    const int32_t id = hs.session.acceptedFrameId();
    const uint32_t crc = hs.session.acceptedCrc();

    std::vector<uint8_t> rx(M4B3::kMaxMessageSize);
    M4B3::StreamParser p;
    p.attach(rx.data(), rx.size());
    auto changed = base;
    setBlack(changed, 0, 0);
    n = encodeOneRectPatch(wire, sizeof(wire), 2, 1, 0, changed, 0, 0, 16, 16);
    assert(p.append(wire, 10) == M4B3::Status::Ok);
    const uint8_t* msg = nullptr;
    size_t msgLen = 0;
    assert(p.nextMessage(&msg, &msgLen) == M4B3::Status::Truncated);
    p.reset();
    assert(hs.session.acceptedFrameId() == id);
    assert(hs.session.acceptedCrc() == crc);
  }

  printf("m4b3 receiver: PASS\n");
  return 0;
}

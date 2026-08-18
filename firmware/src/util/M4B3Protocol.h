#pragma once

// Host-testable M4B3 v1 codec + atomic session. Matches the validated Java
// contract in android/.../browser/stream/M4B3*.java. No Arduino / panel deps.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/M4ScreenFrameCodec.h"

namespace M4B3 {

constexpr char kMagic[4] = {'M', '4', 'B', '3'};
constexpr uint8_t kVersion = 1;
constexpr size_t kEnvelopeSize = 16;

constexpr uint8_t kTypeHello = 1;
constexpr uint8_t kTypeFrameKey = 2;
constexpr uint8_t kTypeFramePatch = 3;
constexpr uint8_t kTypeFrameAck = 4;
constexpr uint8_t kTypePing = 5;
constexpr uint8_t kTypePong = 6;
constexpr uint8_t kTypeTouch = 7;

constexpr uint8_t kPixelMono1 = 1;
constexpr uint16_t kWidth = 480;
constexpr uint16_t kHeight = 800;
constexpr uint16_t kStride = 60;
constexpr uint32_t kKeyframeSize = 48000;

constexpr uint16_t kHelloHeaderSize = 17;
constexpr uint16_t kKeyHeaderSize = 19;
constexpr uint16_t kPatchHeaderSize = 14;
constexpr uint16_t kAckHeaderSize = 9;
constexpr uint16_t kPingHeaderSize = 4;
constexpr uint16_t kTouchHeaderSize = 20;
constexpr uint16_t kRectMetaSize = 12;

// Additive M4→Android single-pointer input. Existing HELLO/KEY/PATCH/ACK
// layouts are unchanged. 0 is reserved so an all-zero header is invalid.
constexpr uint8_t kTouchDown = 1;
constexpr uint8_t kTouchMove = 2;
constexpr uint8_t kTouchUp = 3;
constexpr uint8_t kTouchCancel = 4;

inline bool validTouchAction(uint8_t action) {
  return action >= kTouchDown && action <= kTouchCancel;
}

constexpr uint16_t kMaxHeaderLen = 32;
constexpr uint32_t kMaxPayloadLen = 96u * 1024u;
constexpr uint16_t kMaxRectCount = 1500;
constexpr size_t kMaxMessageSize = kEnvelopeSize + kMaxHeaderLen + kMaxPayloadLen;

constexpr uint32_t kCapMono1 = 1u;
constexpr uint32_t kCapPatch = 1u << 1;
constexpr uint32_t kV1Capabilities = kCapMono1 | kCapPatch;

constexpr uint8_t kHelloOk = 0;
constexpr uint8_t kHelloUnsupportedVersion = 1;
constexpr uint8_t kHelloFormatMismatch = 2;

constexpr uint8_t kAckOk = 0;
constexpr uint8_t kNackCrc = 1;
constexpr uint8_t kNackBase = 2;
constexpr uint8_t kNackMalformed = 3;
constexpr uint8_t kNackOverflow = 4;
constexpr uint8_t kNackVersion = 5;

constexpr uint32_t kCrc32Check = 0xCBF43926u;

enum class Status : uint8_t {
  Ok = 0,
  Truncated,
  Oversized,
  Overflow,
  Invalid,
  Version,
};

inline uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void wr16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

inline void wr32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t crc32(const uint8_t* data, size_t len) { return M4ScreenFrameCodec::crc32(data, len); }

inline bool sameLogicalFormat(uint16_t width, uint16_t height, uint8_t pixel, uint16_t stride) {
  return width == kWidth && height == kHeight && pixel == kPixelMono1 && stride == kStride;
}

inline bool isKnownType(uint8_t type) { return type >= kTypeHello && type <= kTypeTouch; }

inline uint8_t nackFor(Status st) {
  switch (st) {
    case Status::Oversized:
    case Status::Overflow:
      return kNackOverflow;
    case Status::Version:
      return kNackVersion;
    default:
      return kNackMalformed;
  }
}

struct Envelope {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t headerLen = 0;
  uint32_t payloadLen = 0;
  uint32_t seq = 0;
};

inline Status parseEnvelope(const uint8_t* data, size_t len, Envelope& out, size_t* need) {
  if (need) *need = kEnvelopeSize;
  if (!data || len < kEnvelopeSize) return Status::Truncated;
  if (std::memcmp(data, kMagic, 4) != 0) return Status::Invalid;
  const uint8_t type = data[4];
  const uint8_t flags = data[5];
  const uint16_t headerLen = rd16(data + 6);
  const uint32_t payloadLen = rd32(data + 8);
  const uint32_t seq = rd32(data + 12);
  if (headerLen > kMaxHeaderLen) return Status::Oversized;
  if (payloadLen > kMaxPayloadLen) return Status::Oversized;
  const uint64_t body = static_cast<uint64_t>(headerLen) + payloadLen;
  if (body > static_cast<uint64_t>(kMaxHeaderLen) + kMaxPayloadLen) return Status::Overflow;
  const uint64_t total = static_cast<uint64_t>(kEnvelopeSize) + body;
  if (need) *need = static_cast<size_t>(total);
  if (total > len) return Status::Truncated;
  if (!isKnownType(type)) return Status::Invalid;
  out.type = type;
  out.flags = flags;
  out.headerLen = headerLen;
  out.payloadLen = payloadLen;
  out.seq = seq;
  return Status::Ok;
}

inline size_t wrap(uint8_t* out, size_t cap, uint8_t type, uint8_t flags, uint32_t seq, const uint8_t* header,
                   uint16_t headerLen, const uint8_t* payload, uint32_t payloadLen) {
  const size_t total = kEnvelopeSize + static_cast<size_t>(headerLen) + static_cast<size_t>(payloadLen);
  if (!out || total > cap) return 0;
  std::memcpy(out, kMagic, 4);
  out[4] = type;
  out[5] = flags;
  wr16(out + 6, headerLen);
  wr32(out + 8, payloadLen);
  wr32(out + 12, seq);
  if (headerLen) {
    if (!header) return 0;
    std::memcpy(out + kEnvelopeSize, header, headerLen);
  }
  if (payloadLen) {
    if (!payload) return 0;
    std::memcpy(out + kEnvelopeSize + headerLen, payload, payloadLen);
  }
  return total;
}

inline size_t encodeHello(uint8_t* out, size_t cap, uint32_t seq, uint8_t status) {
  uint8_t h[kHelloHeaderSize];
  h[0] = kVersion;
  wr16(h + 1, kWidth);
  wr16(h + 3, kHeight);
  h[5] = kPixelMono1;
  wr16(h + 6, kStride);
  wr32(h + 8, kV1Capabilities);
  wr32(h + 12, kMaxPayloadLen);
  h[16] = status;
  return wrap(out, cap, kTypeHello, 0, seq, h, kHelloHeaderSize, nullptr, 0);
}

inline size_t encodeAck(uint8_t* out, size_t cap, uint32_t seq, uint32_t frameId, uint8_t result,
                        int32_t acceptedFrameId) {
  uint8_t h[kAckHeaderSize];
  wr32(h, frameId);
  h[4] = result;
  wr32(h + 5, static_cast<uint32_t>(acceptedFrameId));
  return wrap(out, cap, kTypeFrameAck, 0, seq, h, kAckHeaderSize, nullptr, 0);
}

inline size_t encodePingPong(uint8_t* out, size_t cap, uint8_t type, uint32_t seq, uint32_t nonce) {
  uint8_t h[kPingHeaderSize];
  wr32(h, nonce);
  return wrap(out, cap, type, 0, seq, h, kPingHeaderSize, nullptr, 0);
}

// TOUCH header (20 B LE): action u8, flags u8, reserved u16, x u16, y u16,
// t_ms u32, input_seq u32, session u32. Payload is empty.
inline size_t encodeTouch(uint8_t* out, size_t cap, uint32_t envSeq, uint8_t action, uint8_t flags,
                          uint16_t x, uint16_t y, uint32_t tMs, uint32_t inputSeq, uint32_t session) {
  uint8_t h[kTouchHeaderSize];
  h[0] = action;
  h[1] = flags;
  wr16(h + 2, 0);
  wr16(h + 4, x);
  wr16(h + 6, y);
  wr32(h + 8, tMs);
  wr32(h + 12, inputSeq);
  wr32(h + 16, session);
  return wrap(out, cap, kTypeTouch, 0, envSeq, h, kTouchHeaderSize, nullptr, 0);
}

inline size_t encodeKeyframe(uint8_t* out, size_t cap, uint32_t seq, uint32_t frameId, const uint8_t* fb) {
  if (!fb) return 0;
  uint8_t h[kKeyHeaderSize];
  wr32(h, frameId);
  wr16(h + 4, kWidth);
  wr16(h + 6, kHeight);
  h[8] = kPixelMono1;
  wr16(h + 9, kStride);
  wr32(h + 11, kKeyframeSize);
  wr32(h + 15, crc32(fb, kKeyframeSize));
  return wrap(out, cap, kTypeFrameKey, 0, seq, h, kKeyHeaderSize, fb, kKeyframeSize);
}

struct RectView {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  const uint8_t* data = nullptr;
  uint32_t bytes = 0;
};

inline Status validateRect(const RectView& r) {
  if ((r.x & 7) != 0 || (r.w & 7) != 0) return Status::Invalid;
  if (r.w == 0 || r.h == 0) return Status::Invalid;
  const uint64_t expected = (static_cast<uint64_t>(r.w) >> 3) * r.h;
  if (expected > kMaxPayloadLen) return Status::Overflow;
  const uint64_t right = static_cast<uint64_t>(r.x) + r.w;
  const uint64_t bottom = static_cast<uint64_t>(r.y) + r.h;
  if (right > kWidth || bottom > kHeight) return Status::Invalid;
  if (r.bytes != expected) return Status::Invalid;
  if (!r.data && r.bytes != 0) return Status::Invalid;
  return Status::Ok;
}

inline Status applyRect(uint8_t* fb, const RectView& r) {
  const Status st = validateRect(r);
  if (st != Status::Ok) return st;
  const uint16_t dstX = static_cast<uint16_t>(r.x >> 3);
  const uint16_t rectStride = static_cast<uint16_t>(r.w >> 3);
  for (uint16_t row = 0; row < r.h; ++row) {
    std::memcpy(fb + (static_cast<size_t>(r.y + row) * kStride) + dstX, r.data + (static_cast<size_t>(row) * rectStride),
                rectStride);
  }
  return Status::Ok;
}

inline Status parseAndApplyPatchPayload(const uint8_t* payload, uint32_t payloadLen, uint16_t rectCount, uint8_t* dest) {
  if (rectCount > kMaxRectCount) return Status::Oversized;
  const uint64_t minNeed = static_cast<uint64_t>(rectCount) * kRectMetaSize;
  if (minNeed > payloadLen) return Status::Truncated;
  size_t cursor = 0;
  uint32_t remaining = payloadLen;
  for (uint16_t i = 0; i < rectCount; ++i) {
    if (remaining < kRectMetaSize) return Status::Truncated;
    RectView r;
    r.x = rd16(payload + cursor);
    r.y = rd16(payload + cursor + 2);
    r.w = rd16(payload + cursor + 4);
    r.h = rd16(payload + cursor + 6);
    r.bytes = rd32(payload + cursor + 8);
    cursor += kRectMetaSize;
    remaining -= kRectMetaSize;
    if (r.bytes > remaining) return Status::Truncated;
    r.data = payload + cursor;
    const Status st = applyRect(dest, r);
    if (st != Status::Ok) return st;
    cursor += r.bytes;
    remaining -= r.bytes;
  }
  if (remaining != 0) return Status::Invalid;
  return Status::Ok;
}

struct StreamParser {
  uint8_t* buf = nullptr;
  size_t cap = 0;
  size_t filled = 0;

  void attach(uint8_t* storage, size_t storageCap) {
    buf = storage;
    cap = storageCap;
    filled = 0;
  }

  void reset() { filled = 0; }

  Status append(const uint8_t* data, size_t n) {
    if (!buf) return Status::Invalid;
    if (n == 0) return Status::Ok;
    if (!data) return Status::Invalid;
    if (n > cap - filled) return Status::Overflow;
    std::memcpy(buf + filled, data, n);
    filled += n;
    return Status::Ok;
  }

  Status nextMessage(const uint8_t** msg, size_t* msgLen) {
    if (msg) *msg = nullptr;
    if (msgLen) *msgLen = 0;
    if (filled < kEnvelopeSize) return Status::Truncated;
    Envelope env;
    size_t need = 0;
    const Status st = parseEnvelope(buf, filled, env, &need);
    if (st == Status::Truncated) return Status::Truncated;
    if (st != Status::Ok) return st;
    if (need > filled) return Status::Truncated;
    if (msg) *msg = buf;
    if (msgLen) *msgLen = need;
    return Status::Ok;
  }

  void consume(size_t n) {
    if (n == 0 || !buf) return;
    if (n >= filled) {
      filled = 0;
      return;
    }
    std::memmove(buf, buf + n, filled - n);
    filled -= n;
  }
};

struct Stats {
  uint32_t keys = 0;
  uint32_t patches = 0;
  uint32_t nacks = 0;
  uint32_t hellos = 0;
  uint32_t pings = 0;
  uint32_t bytesRx = 0;
  uint32_t bytesTx = 0;
  uint32_t applyErrors = 0;
  uint8_t lastNack = 0xFF;
};

// Persistent logical 480x800 MONO1 session. accepted/candidate are caller-owned
// 48,000-byte buffers. Failures never mutate the accepted framebuffer.
class Session {
 public:
  void attach(uint8_t* acceptedFb, uint8_t* candidateFb) {
    accepted_ = acceptedFb;
    candidate_ = candidateFb;
    reset();
  }

  void reset() {
    helloOk_ = false;
    acceptedFrameId_ = -1;
    acceptedCrc_ = 0;
    nextSeq_ = 0;
    stats_ = Stats{};
    if (accepted_) std::memset(accepted_, 0xFF, kKeyframeSize);
    if (candidate_) std::memset(candidate_, 0xFF, kKeyframeSize);
  }

  bool helloOk() const { return helloOk_; }
  int32_t acceptedFrameId() const { return acceptedFrameId_; }
  uint32_t acceptedCrc() const { return acceptedCrc_; }
  const uint8_t* accepted() const { return accepted_; }
  const Stats& stats() const { return stats_; }
  Stats& stats() { return stats_; }

  size_t handle(const uint8_t* msg, size_t len, uint8_t* out, size_t outCap) {
    stats_.bytesRx += static_cast<uint32_t>(len);
    Envelope env;
    size_t need = 0;
    const Status envSt = parseEnvelope(msg, len, env, &need);
    if (envSt != Status::Ok || need != len) {
      stats_.applyErrors++;
      return 0;
    }
    const uint8_t* header = msg + kEnvelopeSize;
    const uint8_t* payload = header + env.headerLen;
    size_t n = 0;
    switch (env.type) {
      case kTypeHello:
        n = onHello(header, env.headerLen, env.payloadLen, out, outCap);
        break;
      case kTypeFrameKey:
        n = onKey(header, env.headerLen, payload, env.payloadLen, out, outCap);
        break;
      case kTypeFramePatch:
        n = onPatch(header, env.headerLen, payload, env.payloadLen, out, outCap);
        break;
      case kTypePing:
        n = onPing(header, env.headerLen, env.payloadLen, out, outCap);
        break;
      case kTypePong:
      case kTypeFrameAck:
      case kTypeTouch:
        return 0;
      default:
        stats_.applyErrors++;
        return 0;
    }
    stats_.bytesTx += static_cast<uint32_t>(n);
    return n;
  }

 private:
  size_t replyAck(uint8_t* out, size_t outCap, uint32_t frameId, uint8_t result) {
    return encodeAck(out, outCap, nextSeq_++, frameId, result, acceptedFrameId_);
  }

  size_t nack(uint8_t* out, size_t outCap, uint32_t frameId, uint8_t result) {
    stats_.nacks++;
    stats_.lastNack = result;
    return replyAck(out, outCap, frameId, result);
  }

  void commit(uint32_t frameId, uint32_t crc) {
    std::memcpy(accepted_, candidate_, kKeyframeSize);
    acceptedFrameId_ = static_cast<int32_t>(frameId);
    acceptedCrc_ = crc;
    helloOk_ = true;
  }

  size_t onHello(const uint8_t* header, uint16_t headerLen, uint32_t payloadLen, uint8_t* out, size_t outCap) {
    stats_.hellos++;
    uint8_t status = kHelloOk;
    if (payloadLen != 0 || headerLen != kHelloHeaderSize) {
      helloOk_ = false;
      status = kHelloFormatMismatch;
      return encodeHello(out, outCap, nextSeq_++, status);
    }
    const uint8_t version = header[0];
    const uint16_t width = rd16(header + 1);
    const uint16_t height = rd16(header + 3);
    const uint8_t pixel = header[5];
    const uint16_t stride = rd16(header + 6);
    if (version != kVersion) {
      helloOk_ = false;
      status = kHelloUnsupportedVersion;
    } else if (!sameLogicalFormat(width, height, pixel, stride)) {
      helloOk_ = false;
      status = kHelloFormatMismatch;
    } else {
      helloOk_ = true;
    }
    return encodeHello(out, outCap, nextSeq_++, status);
  }

  size_t onKey(const uint8_t* header, uint16_t headerLen, const uint8_t* payload, uint32_t payloadLen, uint8_t* out,
               size_t outCap) {
    if (!accepted_ || !candidate_) {
      stats_.applyErrors++;
      return 0;
    }
    if (headerLen != kKeyHeaderSize) {
      return nack(out, outCap, 0, kNackMalformed);
    }
    const uint32_t frameId = rd32(header);
    const uint16_t width = rd16(header + 4);
    const uint16_t height = rd16(header + 6);
    const uint8_t pixel = header[8];
    const uint16_t stride = rd16(header + 9);
    const uint32_t claimed = rd32(header + 11);
    const uint32_t crc = rd32(header + 15);
    if (claimed != payloadLen || !sameLogicalFormat(width, height, pixel, stride) || claimed != kKeyframeSize) {
      return nack(out, outCap, frameId, claimed > kMaxPayloadLen ? kNackOverflow : kNackMalformed);
    }
    std::memcpy(candidate_, payload, kKeyframeSize);
    const uint32_t got = crc32(candidate_, kKeyframeSize);
    if (got != crc) return nack(out, outCap, frameId, kNackCrc);
    commit(frameId, got);
    stats_.keys++;
    return replyAck(out, outCap, frameId, kAckOk);
  }

  size_t onPatch(const uint8_t* header, uint16_t headerLen, const uint8_t* payload, uint32_t payloadLen, uint8_t* out,
                 size_t outCap) {
    if (!accepted_ || !candidate_) {
      stats_.applyErrors++;
      return 0;
    }
    if (headerLen != kPatchHeaderSize) {
      return nack(out, outCap, 0, kNackMalformed);
    }
    const uint32_t frameId = rd32(header);
    const uint32_t baseId = rd32(header + 4);
    const uint16_t rectCount = rd16(header + 8);
    const uint32_t crc = rd32(header + 10);
    if (acceptedFrameId_ < 0 || static_cast<uint32_t>(acceptedFrameId_) != baseId) {
      return nack(out, outCap, frameId, kNackBase);
    }
    std::memcpy(candidate_, accepted_, kKeyframeSize);
    const Status st = parseAndApplyPatchPayload(payload, payloadLen, rectCount, candidate_);
    if (st != Status::Ok) {
      stats_.applyErrors++;
      return nack(out, outCap, frameId, nackFor(st));
    }
    const uint32_t got = crc32(candidate_, kKeyframeSize);
    if (got != crc) return nack(out, outCap, frameId, kNackCrc);
    commit(frameId, got);
    stats_.patches++;
    return replyAck(out, outCap, frameId, kAckOk);
  }

  size_t onPing(const uint8_t* header, uint16_t headerLen, uint32_t payloadLen, uint8_t* out, size_t outCap) {
    if (payloadLen != 0 || headerLen != kPingHeaderSize) return 0;
    stats_.pings++;
    return encodePingPong(out, outCap, kTypePong, nextSeq_++, rd32(header));
  }

  uint8_t* accepted_ = nullptr;
  uint8_t* candidate_ = nullptr;
  bool helloOk_ = false;
  int32_t acceptedFrameId_ = -1;
  uint32_t acceptedCrc_ = 0;
  uint32_t nextSeq_ = 0;
  Stats stats_{};
};

}  // namespace M4B3

#pragma once

// M4 screen-bridge wire codec: /v1/page response framing, custom RLE1 decode,
// IEEE CRC-32 over the decoded framebuffer. Host-testable; no Arduino deps.
// See docs/SCREEN_BRIDGE_PROTOCOL.md for the full wire format.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace M4ScreenFrameCodec {

// Wire constants (fixed physical panel).
constexpr char kMagic[4] = {'M', '4', 'R', '1'};
constexpr uint8_t kVersion = 1;
constexpr uint8_t kCodecRaw = 0;   // raw 1bpp framebuffer
constexpr uint8_t kCodecRle1 = 1;  // custom run/literal RLE
constexpr uint16_t kWidth = 800;
constexpr uint16_t kHeight = 480;
constexpr uint16_t kStride = 100;  // bytes per row (800 px / 8)
constexpr uint32_t kRawSize = 48000;
constexpr size_t kHeaderSize = 24;

// Header layout (all little-endian): magic[4] version u8 codec u8 width u16
// height u16 stride u16 page i32 rawSize u32 crc32 u32.
struct FrameHeader {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t stride = 0;
  int32_t page = -1;
  uint32_t rawSize = 0;
  uint8_t codec = 0;
  uint32_t crc32 = 0;
};

inline bool parseHeader(const uint8_t* d, size_t len, FrameHeader& out) {
  if (!d || len < kHeaderSize) return false;
  if (std::memcmp(d, kMagic, 4) != 0) return false;
  if (d[4] != kVersion) return false;
  const uint8_t codec = d[5];
  if (codec != kCodecRaw && codec != kCodecRle1) return false;
  const uint16_t width = static_cast<uint16_t>(d[6] | (d[7] << 8));
  const uint16_t height = static_cast<uint16_t>(d[8] | (d[9] << 8));
  const uint16_t stride = static_cast<uint16_t>(d[10] | (d[11] << 8));
  const int32_t page =
      static_cast<int32_t>(static_cast<uint32_t>(d[12]) | (static_cast<uint32_t>(d[13]) << 8) |
                           (static_cast<uint32_t>(d[14]) << 16) | (static_cast<uint32_t>(d[15]) << 24));
  const uint32_t rawSize = static_cast<uint32_t>(d[16]) | (static_cast<uint32_t>(d[17]) << 8) |
                           (static_cast<uint32_t>(d[18]) << 16) | (static_cast<uint32_t>(d[19]) << 24);
  const uint32_t crc = static_cast<uint32_t>(d[20]) | (static_cast<uint32_t>(d[21]) << 8) |
                       (static_cast<uint32_t>(d[22]) << 16) | (static_cast<uint32_t>(d[23]) << 24);
  if (width != kWidth || height != kHeight || stride != kStride || rawSize != kRawSize) {
    return false;
  }
  out.width = width;
  out.height = height;
  out.stride = stride;
  out.page = page;
  out.rawSize = rawSize;
  out.codec = codec;
  out.crc32 = crc;
  return true;
}

// IEEE CRC-32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF). Check value of
// "123456789" is 0xCBF43926.
inline uint32_t crc32(const uint8_t* data, size_t len) {
  if (!data || len == 0) return 0;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// RLE1: token high bit set => run of (low7+3) of one repeated byte; token high
// bit clear => literal of (low7+1) raw bytes. Decode must produce exactly
// `expected` bytes; fails on truncated input, capacity overflow, or size
// mismatch.
inline bool decodeRle1(const uint8_t* src, size_t srcLen, uint8_t* out, size_t outCap,
                       size_t expected) {
  if (!src || !out) return false;
  size_t sp = 0, op = 0;
  while (sp < srcLen) {
    const uint8_t token = src[sp++];
    if (token & 0x80) {
      const size_t run = static_cast<size_t>(token & 0x7F) + 3;
      if (sp >= srcLen) return false;
      const uint8_t value = src[sp++];
      if (run > outCap - op) return false;
      std::memset(out + op, value, run);
      op += run;
    } else {
      const size_t lit = static_cast<size_t>(token) + 1;
      if (lit > srcLen - sp) return false;
      if (lit > outCap - op) return false;
      std::memcpy(out + op, src + sp, lit);
      sp += lit;
      op += lit;
    }
  }
  return op == expected;
}

// Decode a full /v1/page body (header + payload), verifying header, codec and
// CRC of the decoded framebuffer. `out` must hold >= rawSize bytes.
inline bool decodePage(const uint8_t* body, size_t bodyLen, uint8_t* out, size_t outCap) {
  FrameHeader h;
  if (!parseHeader(body, bodyLen, h)) return false;
  const size_t payloadLen = bodyLen - kHeaderSize;
  const uint8_t* payload = body + kHeaderSize;
  if (h.codec == kCodecRaw) {
    if (payloadLen != h.rawSize || h.rawSize > outCap) return false;
    std::memcpy(out, payload, h.rawSize);
  } else {
    if (!decodeRle1(payload, payloadLen, out, outCap, h.rawSize)) return false;
  }
  return crc32(out, h.rawSize) == h.crc32;
}

}  // namespace M4ScreenFrameCodec

// Host tests for M4ScreenFrameCodec: header parse/validation, RLE1 decode,
// IEEE CRC-32, and full page decode (raw + RLE, positive and negative paths).
// Build: g++-14 -std=c++14 -I src tests/native_app/test_screen_frame_codec.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "util/M4ScreenFrameCodec.h"

using namespace M4ScreenFrameCodec;

namespace {

std::string le16(uint16_t v) {
  return std::string(1, static_cast<char>(v & 0xFF)) +
         std::string(1, static_cast<char>((v >> 8) & 0xFF));
}

std::string le32(uint32_t v) {
  return std::string(1, static_cast<char>(v & 0xFF)) +
         std::string(1, static_cast<char>((v >> 8) & 0xFF)) +
         std::string(1, static_cast<char>((v >> 16) & 0xFF)) +
         std::string(1, static_cast<char>((v >> 24) & 0xFF));
}

std::string lei32(int32_t v) { return le32(static_cast<uint32_t>(v)); }

std::string frameCrc(const std::string& frame) {
  return le32(crc32(reinterpret_cast<const uint8_t*>(frame.data()), frame.size()));
}

std::string buildHeader(int32_t page, uint8_t codec, const std::string& frame) {
  std::string b("M4R1", 4);
  b += '\x01';
  b += static_cast<char>(codec);
  b += le16(800);
  b += le16(480);
  b += le16(100);
  b += lei32(page);
  b += le32(kRawSize);
  b += frameCrc(frame);
  return b;
}

size_t runAt(const std::string& raw, size_t i) {
  size_t j = i;
  while (j < raw.size() && raw[j] == raw[i]) ++j;
  return j - i;
}

// Minimal RLE1 encoder for tests; the production encoder lives in the phone app.
std::string encodeRle1(const std::string& raw) {
  std::string out;
  size_t i = 0;
  while (i < raw.size()) {
    const size_t run = runAt(raw, i);
    if (run >= 3) {
      const size_t n = run < 130 ? run : 130;
      out += static_cast<char>(0x80 | (n - 3));
      out += raw[i];
      i += n;
    } else {
      size_t lit = 0;
      while (i + lit < raw.size() && lit < 128 && runAt(raw, i + lit) < 3) ++lit;
      out += static_cast<char>(lit - 1);
      out.append(raw, i, lit);
      i += lit;
    }
  }
  return out;
}

// Text-page-like framebuffer: mostly-white rows with repeated black glyph runs,
// so the run-heavy frame actually compresses under RLE1.
std::string makeTextFrame() {
  std::string f;
  f.reserve(kRawSize);
  for (int row = 0; row < 480; ++row) {
    f.append(90, static_cast<char>(0xFF));
    const char glyph = static_cast<char>(((row % 7) << 4) | (row % 7));
    f.append(10, glyph);
  }
  assert(f.size() == kRawSize);
  return f;
}

}  // namespace

int main() {
  // IEEE CRC-32 reference vector and empty-input value.
  assert(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
  assert(crc32(nullptr, 0) == 0u);

  const std::string raw = makeTextFrame();
  const std::string rle = encodeRle1(raw);
  assert(rle.size() < raw.size());  // run-heavy page must compress

  // --- header parse/validation ---
  FrameHeader h;
  std::string body = buildHeader(7, kCodecRaw, raw);
  body += raw;
  assert(parseHeader(reinterpret_cast<const uint8_t*>(body.data()), body.size(), h));
  assert(h.width == 800 && h.height == 480 && h.stride == 100);
  assert(h.rawSize == kRawSize && h.page == 7 && h.codec == kCodecRaw);

  body = buildHeader(-42, kCodecRle1, raw) + rle;
  assert(parseHeader(reinterpret_cast<const uint8_t*>(body.data()), body.size(), h));
  assert(h.page == -42 && h.codec == kCodecRle1);

  std::string bad;
  auto asBytes = [](const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); };
  assert(!parseHeader(asBytes(body), 23, h));  // shorter than 24-byte header
  assert(!parseHeader(nullptr, body.size(), h));
  bad = body; bad[0] = 'X'; assert(!parseHeader(asBytes(bad), bad.size(), h));
  bad = body; bad[1] = 'x'; assert(!parseHeader(asBytes(bad), bad.size(), h));
  bad = body; bad[4] = 2;   assert(!parseHeader(asBytes(bad), bad.size(), h));
  bad = body; bad[5] = 2;   assert(!parseHeader(asBytes(bad), bad.size(), h));  // unknown codec
  bad = body; bad[6] = 0x21; assert(!parseHeader(asBytes(bad), bad.size(), h));  // width 801
  bad = body; bad[8] = 0xE1; assert(!parseHeader(asBytes(bad), bad.size(), h));  // height 481
  bad = body; bad[10] = 0x63; assert(!parseHeader(asBytes(bad), bad.size(), h));  // stride 99
  bad = body; bad[16] = 0xFF; assert(!parseHeader(asBytes(bad), bad.size(), h));  // rawSize

  // --- RLE1 token decode ---
  {
    uint8_t out[200];
    const uint8_t runOnly[] = {0x80, 0xAA};  // run of 3
    assert(decodeRle1(runOnly, sizeof(runOnly), out, sizeof(out), 3));
    assert(out[0] == 0xAA && out[1] == 0xAA && out[2] == 0xAA);

    const uint8_t litOnly[] = {0x02, 'a', 'b', 'c'};  // literal of 3
    assert(decodeRle1(litOnly, sizeof(litOnly), out, sizeof(out), 3));
    assert(out[0] == 'a' && out[1] == 'b' && out[2] == 'c');

    const uint8_t maxRun[] = {0xFF, 0x00};  // run of 130
    assert(decodeRle1(maxRun, sizeof(maxRun), out, 130, 130));
    assert(out[129] == 0x00);

    std::vector<uint8_t> maxLit;  // literal of 128
    maxLit.push_back(0x7F);
    maxLit.resize(129, 0x77);
    assert(decodeRle1(maxLit.data(), maxLit.size(), out, sizeof(out), 128));
    assert(out[127] == 0x77);
  }

  // --- RLE1 malformed / truncated / overflow ---
  {
    uint8_t out[8];
    const uint8_t missingRunByte[] = {0x80};
    assert(!decodeRle1(missingRunByte, sizeof(missingRunByte), out, sizeof(out), 3));
    const uint8_t truncatedLit[] = {0x02, 'a'};
    assert(!decodeRle1(truncatedLit, sizeof(truncatedLit), out, sizeof(out), 3));
    const uint8_t overflowRun[] = {0x80, 0xAA};
    assert(!decodeRle1(overflowRun, sizeof(overflowRun), out, 2, 3));  // capacity 2
    const uint8_t wrongSize[] = {0x80, 0xAA};
    assert(!decodeRle1(wrongSize, sizeof(wrongSize), out, sizeof(out), 4));
    const uint8_t trailing[] = {0x80, 0xAA, 0x80, 0xBB};
    assert(!decodeRle1(trailing, sizeof(trailing), out, sizeof(out), 3));
    const uint8_t* nullPtr = nullptr;
    assert(!decodeRle1(nullPtr, 0, out, sizeof(out), 0));
  }

  // --- full page decode: raw round trip ---
  {
    body = buildHeader(0, kCodecRaw, raw) + raw;
    std::vector<uint8_t> out(kRawSize);
    assert(decodePage(asBytes(body), body.size(), out.data(), out.size()));
    assert(std::memcmp(out.data(), raw.data(), kRawSize) == 0);
  }

  // --- full page decode: RLE round trip ---
  {
    body = buildHeader(1, kCodecRle1, raw) + rle;
    std::vector<uint8_t> out(kRawSize);
    assert(decodePage(asBytes(body), body.size(), out.data(), out.size()));
    assert(std::memcmp(out.data(), raw.data(), kRawSize) == 0);
  }

  // --- full page decode: negative paths ---
  {
    std::string tampered = buildHeader(1, kCodecRle1, raw) + rle;
    tampered.back() ^= 0x01;  // flip one payload bit
    std::vector<uint8_t> out(kRawSize);
    assert(!decodePage(asBytes(tampered), tampered.size(), out.data(), out.size()));

    tampered = buildHeader(0, kCodecRaw, raw) + raw;
    tampered[kHeaderSize + 12345] ^= 0x80;  // corrupt payload byte, keep CRC wrong
    assert(!decodePage(asBytes(tampered), tampered.size(), out.data(), out.size()));

    std::string shortRaw = buildHeader(0, kCodecRaw, raw) + raw;
    shortRaw.erase(shortRaw.size() - 1, 1);  // raw payload one byte short
    assert(!decodePage(asBytes(shortRaw), shortRaw.size(), out.data(), out.size()));

    std::string rleGarbage = buildHeader(1, kCodecRle1, raw) + rle + std::string("\x00x", 2);
    assert(!decodePage(asBytes(rleGarbage), rleGarbage.size(), out.data(), out.size()));

    std::string badCrc = buildHeader(1, kCodecRle1, raw) + rle;
    badCrc[23] ^= 0x01;  // corrupt CRC field
    assert(!decodePage(asBytes(badCrc), badCrc.size(), out.data(), out.size()));

    std::vector<uint8_t> tiny(kRawSize - 1);  // output buffer too small
    body = buildHeader(1, kCodecRle1, raw) + rle;
    assert(!decodePage(asBytes(body), body.size(), tiny.data(), tiny.size()));
  }

  // --- compression choice policy ---
  {
    std::string noisy;
    noisy.resize(kRawSize);
    uint32_t seed = 0x12345678u;
    for (size_t i = 0; i < noisy.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      noisy[i] = static_cast<char>((seed >> 24) & 0xFF);
    }
    assert(encodeRle1(noisy).size() > noisy.size());  // phone must send raw
  }

  printf("screen frame codec: PASS\n");
  return 0;
}

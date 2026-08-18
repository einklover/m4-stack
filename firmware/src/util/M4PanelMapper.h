#pragma once

// Platform-independent mapper: M4B3 logical 480x800 MONO1 -> Murphy M4
// panel-native 800x480 MONO1 framebuffer used by HalDisplay / GfxRenderer /
// SSD1677. Does not mutate the transport/session framebuffer and does not
// drive the panel or select waveforms.
//
// Proven from current production/SDK code (not inferred from dimensions):
//
//   BoardConfig::MURPHY_M4
//     displayWidth=800, displayHeight=480, SSD1677, orientation=NO_FLIP
//   FreeInkDisplay / EInkDisplay
//     DISPLAY_WIDTH=800, DISPLAY_HEIGHT=480, DISPLAY_WIDTH_BYTES=100,
//     BUFFER_SIZE=48000
//   HalDisplay (non-X3 / Murphy M4)
//     same 800x480 physical geometry
//   GfxRenderer default Orientation::Portrait (ctor + CrossPointSettings
//     ORIENTATION::PORTRAIT=0). rotateCoordinates():
//       phyX = logicalY
//       phyY = DISPLAY_HEIGHT - 1 - logicalX     // 479 - logicalX
//     comment in GfxRenderer.cpp: "Rotation: 90 degrees clockwise"
//   GfxRenderer::drawPixel packing (production display buffer):
//       byteIndex   = phyY * DISPLAY_WIDTH_BYTES + (phyX / 8)
//       bitPosition = 7 - (phyX % 8)             // MSB first
//       ink/black clears the bit; 1=white, 0=black
//   M4B3 / LogicalMonoFrame
//     480x800, stride 60, 48000 B, MSB first, 1=white, 0=black
//   Ssd1677Driver::writeRam
//     copies the 800x480 production buffer linearly. Murphy M4 is NO_FLIP;
//     the driver's setRamArea Y-gate reverse is hardware scan compensation
//     and is not a second software rotate of this buffer.
//
// Inverse (for tests / later dirty-region mapping):
//   logicalX = DISPLAY_HEIGHT - 1 - phyY
//   logicalY = phyX

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace M4PanelMapper {

constexpr uint16_t kLogicalWidth = 480;
constexpr uint16_t kLogicalHeight = 800;
constexpr uint16_t kLogicalStride = 60;  // 480 / 8
constexpr uint32_t kLogicalSize = 48000;

constexpr uint16_t kPhysicalWidth = 800;
constexpr uint16_t kPhysicalHeight = 480;
constexpr uint16_t kPhysicalStride = 100;  // 800 / 8
constexpr uint32_t kPhysicalSize = 48000;

// Shared MONO1 packing on both planes.
constexpr bool kMsbFirst = true;
constexpr uint8_t kWhiteFill = 0xFF;
enum class Polarity : uint8_t { BlackIsZero = 0 };
constexpr Polarity kPolarity = Polarity::BlackIsZero;

static_assert((kLogicalWidth % 8) == 0, "logical width must be byte-aligned");
static_assert((kPhysicalWidth % 8) == 0, "physical width must be byte-aligned");
static_assert(kLogicalStride * kLogicalHeight == kLogicalSize, "logical size");
static_assert(kPhysicalStride * kPhysicalHeight == kPhysicalSize, "physical size");
static_assert(kLogicalSize == 48000u && kPhysicalSize == 48000u, "both planes are 48,000 bytes");

enum class Status : uint8_t {
  Ok = 0,
  NullPointer,
  WrongLogicalSize,
  WrongPhysicalSize,
};

// GfxRenderer::Portrait (default). Equations from GfxRenderer.cpp:89-90.
inline void logicalToPhysical(int logicalX, int logicalY, int* physicalX, int* physicalY) {
  *physicalX = logicalY;
  *physicalY = static_cast<int>(kPhysicalHeight) - 1 - logicalX;
}

inline void physicalToLogical(int physicalX, int physicalY, int* logicalX, int* logicalY) {
  *logicalX = static_cast<int>(kPhysicalHeight) - 1 - physicalY;
  *logicalY = physicalX;
}

inline uint8_t msbMask(int x) { return static_cast<uint8_t>(0x80u >> (x & 7)); }

inline bool isBlack(const uint8_t* fb, uint16_t stride, int x, int y) {
  const size_t off = static_cast<size_t>(y) * stride + static_cast<size_t>(x >> 3);
  return (fb[off] & msbMask(x)) == 0;
}

inline void setBlack(uint8_t* fb, uint16_t stride, int x, int y, bool black) {
  const size_t off = static_cast<size_t>(y) * stride + static_cast<size_t>(x >> 3);
  if (black) {
    fb[off] = static_cast<uint8_t>(fb[off] & static_cast<uint8_t>(~msbMask(x)));
  } else {
    fb[off] = static_cast<uint8_t>(fb[off] | msbMask(x));
  }
}

// logical[y][x] -> physical[479-x][y], MSB-first, black=0.
inline Status mapLogicalToPhysical(const uint8_t* logical, size_t logicalLen, uint8_t* physical,
                                   size_t physicalCap, size_t* written = nullptr) {
  if (written) *written = 0;
  if (!logical || !physical) return Status::NullPointer;
  if (logicalLen != kLogicalSize) return Status::WrongLogicalSize;
  if (physicalCap < kPhysicalSize) return Status::WrongPhysicalSize;

  for (int py = 0; py < static_cast<int>(kPhysicalHeight); ++py) {
    const int lx = static_cast<int>(kPhysicalHeight) - 1 - py;
    const int lByte = lx >> 3;
    const uint8_t lMask = msbMask(lx);
    uint8_t* dst = physical + static_cast<size_t>(py) * kPhysicalStride;
    for (int pByte = 0; pByte < static_cast<int>(kPhysicalStride); ++pByte) {
      uint8_t out = 0;
      const int y0 = pByte * 8;
      for (int b = 0; b < 8; ++b) {
        const uint8_t src = logical[static_cast<size_t>(y0 + b) * kLogicalStride + static_cast<size_t>(lByte)];
        if (src & lMask) out = static_cast<uint8_t>(out | msbMask(b));
      }
      dst[pByte] = out;
    }
  }
  if (written) *written = kPhysicalSize;
  return Status::Ok;
}

// Inverse of mapLogicalToPhysical. Used by tests to prove identity; a pair of
// identically wrong transforms can still round-trip, so tests also check
// absolute expected positions.
inline Status mapPhysicalToLogical(const uint8_t* physical, size_t physicalLen, uint8_t* logical,
                                   size_t logicalCap, size_t* written = nullptr) {
  if (written) *written = 0;
  if (!physical || !logical) return Status::NullPointer;
  if (physicalLen != kPhysicalSize) return Status::WrongPhysicalSize;
  if (logicalCap < kLogicalSize) return Status::WrongLogicalSize;

  for (int ly = 0; ly < static_cast<int>(kLogicalHeight); ++ly) {
    const int px = ly;
    const int pByte = px >> 3;
    const uint8_t pMask = msbMask(px);
    uint8_t* dst = logical + static_cast<size_t>(ly) * kLogicalStride;
    for (int lByte = 0; lByte < static_cast<int>(kLogicalStride); ++lByte) {
      uint8_t out = 0;
      const int x0 = lByte * 8;
      for (int b = 0; b < 8; ++b) {
        const int py = static_cast<int>(kPhysicalHeight) - 1 - (x0 + b);
        const uint8_t src = physical[static_cast<size_t>(py) * kPhysicalStride + static_cast<size_t>(pByte)];
        if (src & pMask) out = static_cast<uint8_t>(out | msbMask(b));
      }
      dst[lByte] = out;
    }
  }
  if (written) *written = kLogicalSize;
  return Status::Ok;
}

}  // namespace M4PanelMapper

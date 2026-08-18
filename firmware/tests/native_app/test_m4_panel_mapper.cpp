// Host tests for M4PanelMapper: 480x800 logical MONO1 -> 800x480 panel MONO1.
// Build: g++ -std=c++14 -Wall -Wextra -Werror -I firmware/src
//        firmware/tests/native_app/test_m4_panel_mapper.cpp -o /tmp/test_m4_panel_mapper
//
// Expected positions are computed from the GfxRenderer.cpp Portrait equations
// independently of M4PanelMapper helpers so a wrong helper cannot hide a
// wrong mapper.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "util/M4PanelMapper.h"

namespace {

// Independent copy of GfxRenderer.cpp:89-90 (Portrait, 90° CW).
void expectedPhy(int lx, int ly, int* px, int* py) {
  *px = ly;
  *py = 480 - 1 - lx;
}

// Independent copy of GfxRenderer::drawPixel packing (MSB first, 1=white).
size_t phyByteIndex(int px, int py) {
  return static_cast<size_t>(py) * 100u + static_cast<size_t>(px / 8);
}

uint8_t phyBitMask(int px) { return static_cast<uint8_t>(0x80u >> (px % 8)); }

bool phyIsBlack(const std::vector<uint8_t>& fb, int px, int py) {
  return (fb[phyByteIndex(px, py)] & phyBitMask(px)) == 0;
}

size_t logByteIndex(int lx, int ly) {
  return static_cast<size_t>(ly) * 60u + static_cast<size_t>(lx / 8);
}

uint8_t logBitMask(int lx) { return static_cast<uint8_t>(0x80u >> (lx % 8)); }

void logSetBlack(std::vector<uint8_t>& fb, int lx, int ly) {
  fb[logByteIndex(lx, ly)] =
      static_cast<uint8_t>(fb[logByteIndex(lx, ly)] & static_cast<uint8_t>(~logBitMask(lx)));
}

bool logIsBlack(const std::vector<uint8_t>& fb, int lx, int ly) {
  return (fb[logByteIndex(lx, ly)] & logBitMask(lx)) == 0;
}

std::vector<uint8_t> whiteLogical() { return std::vector<uint8_t>(48000, 0xFF); }

std::vector<uint8_t> mapOrDie(const std::vector<uint8_t>& logical) {
  std::vector<uint8_t> physical(48000, 0x00);
  size_t written = 0;
  const M4PanelMapper::Status st =
      M4PanelMapper::mapLogicalToPhysical(logical.data(), logical.size(), physical.data(),
                                          physical.size(), &written);
  assert(st == M4PanelMapper::Status::Ok);
  assert(written == 48000u);
  assert(physical.size() == 48000u);
  return physical;
}

void expectBlackAt(const std::vector<uint8_t>& physical, int lx, int ly) {
  int px = 0;
  int py = 0;
  expectedPhy(lx, ly, &px, &py);
  assert(px >= 0 && px < 800);
  assert(py >= 0 && py < 480);
  assert(phyIsBlack(physical, px, py));
}

void expectWhiteAtPhy(const std::vector<uint8_t>& physical, int px, int py) {
  assert(!phyIsBlack(physical, px, py));
}

int countBlack(const std::vector<uint8_t>& fb) {
  int n = 0;
  for (uint8_t b : fb) n += __builtin_popcount(static_cast<unsigned>(~b) & 0xFFu);
  return n;
}

uint32_t lcg(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

}  // namespace

int main() {
  using M4PanelMapper::kLogicalHeight;
  using M4PanelMapper::kLogicalSize;
  using M4PanelMapper::kLogicalStride;
  using M4PanelMapper::kLogicalWidth;
  using M4PanelMapper::kPhysicalHeight;
  using M4PanelMapper::kPhysicalSize;
  using M4PanelMapper::kPhysicalStride;
  using M4PanelMapper::kPhysicalWidth;
  using M4PanelMapper::Status;

  assert(kLogicalWidth == 480);
  assert(kLogicalHeight == 800);
  assert(kLogicalStride == 60);
  assert(kLogicalSize == 48000);
  assert(kPhysicalWidth == 800);
  assert(kPhysicalHeight == 480);
  assert(kPhysicalStride == 100);
  assert(kPhysicalSize == 48000);
  assert(M4PanelMapper::kMsbFirst);
  assert(M4PanelMapper::kPolarity == M4PanelMapper::Polarity::BlackIsZero);

  // Fail-safe size / null checks. Do not write an output on failure.
  {
    std::vector<uint8_t> in(48000, 0xFF);
    std::vector<uint8_t> out(48000, 0xAA);
    size_t written = 99;
    assert(M4PanelMapper::mapLogicalToPhysical(nullptr, 48000, out.data(), 48000, &written) ==
           Status::NullPointer);
    assert(written == 0);
    assert(M4PanelMapper::mapLogicalToPhysical(in.data(), 48000, nullptr, 48000, &written) ==
           Status::NullPointer);
    assert(M4PanelMapper::mapLogicalToPhysical(in.data(), 47999, out.data(), 48000, &written) ==
           Status::WrongLogicalSize);
    assert(written == 0);
    assert(M4PanelMapper::mapLogicalToPhysical(in.data(), 48001, out.data(), 48000, &written) ==
           Status::WrongLogicalSize);
    assert(M4PanelMapper::mapLogicalToPhysical(in.data(), 48000, out.data(), 47999, &written) ==
           Status::WrongPhysicalSize);
    assert(written == 0);
    for (uint8_t b : out) assert(b == 0xAA);
  }

  // All-white identity of fill / polarity.
  {
    const std::vector<uint8_t> physical = mapOrDie(whiteLogical());
    assert(physical.size() == 48000);
    for (uint8_t b : physical) assert(b == 0xFF);
    assert(countBlack(physical) == 0);
  }

  // Four distinct corners. Independent expected physical bytes:
  //   (0,0)     -> (0,479)   byte 47900 bit7
  //   (479,0)   -> (0,0)     byte 0     bit7
  //   (0,799)   -> (799,479) byte 47999 bit0
  //   (479,799) -> (799,0)   byte 99    bit0
  {
    std::vector<uint8_t> logical = whiteLogical();
    logSetBlack(logical, 0, 0);
    logSetBlack(logical, 479, 0);
    logSetBlack(logical, 0, 799);
    logSetBlack(logical, 479, 799);
    const std::vector<uint8_t> physical = mapOrDie(logical);
    assert(countBlack(physical) == 4);

    expectBlackAt(physical, 0, 0);
    expectBlackAt(physical, 479, 0);
    expectBlackAt(physical, 0, 799);
    expectBlackAt(physical, 479, 799);

    assert(physical[47900] == static_cast<uint8_t>(0x7F));  // (0,0)->(0,479) bit7
    assert(physical[0] == static_cast<uint8_t>(0x7F));      // (479,0)->(0,0) bit7
    assert(physical[47999] == static_cast<uint8_t>(0xFE));  // (0,799)->(799,479) bit0
    assert(physical[99] == static_cast<uint8_t>(0xFE));     // (479,799)->(799,0) bit0

    // Neighbours stay white: identity / 90° CCW / mirror would miss these.
    expectWhiteAtPhy(physical, 1, 479);
    expectWhiteAtPhy(physical, 0, 478);
    expectWhiteAtPhy(physical, 1, 0);
    expectWhiteAtPhy(physical, 0, 1);
    expectWhiteAtPhy(physical, 798, 479);
    expectWhiteAtPhy(physical, 799, 478);
    expectWhiteAtPhy(physical, 798, 0);
    expectWhiteAtPhy(physical, 799, 1);
  }

  // Unequal A/B/C marker blocks at asymmetric locations.
  {
    std::vector<uint8_t> logical = whiteLogical();
    // A: 8x16 at (8,16)
    for (int y = 16; y < 32; ++y)
      for (int x = 8; x < 16; ++x) logSetBlack(logical, x, y);
    // B: 16x8 at (40,200)
    for (int y = 200; y < 208; ++y)
      for (int x = 40; x < 56; ++x) logSetBlack(logical, x, y);
    // C: 24x24 at (200,400)
    for (int y = 400; y < 424; ++y)
      for (int x = 200; x < 224; ++x) logSetBlack(logical, x, y);

    const int expectA = 8 * 16;
    const int expectB = 16 * 8;
    const int expectC = 24 * 24;
    assert(countBlack(logical) == expectA + expectB + expectC);

    const std::vector<uint8_t> physical = mapOrDie(logical);
    assert(countBlack(physical) == expectA + expectB + expectC);

    auto checkBlock = [&](int x0, int y0, int w, int h) {
      for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
          expectBlackAt(physical, x, y);
        }
      }
      // One-pixel halo outside the block must stay white (catches extra extent).
      int px = 0, py = 0;
      expectedPhy(x0 - 1, y0, &px, &py);
      if (x0 > 0) expectWhiteAtPhy(physical, px, py);
      expectedPhy(x0 + w, y0, &px, &py);
      if (x0 + w < 480) expectWhiteAtPhy(physical, px, py);
      expectedPhy(x0, y0 - 1, &px, &py);
      if (y0 > 0) expectWhiteAtPhy(physical, px, py);
      expectedPhy(x0, y0 + h, &px, &py);
      if (y0 + h < 800) expectWhiteAtPhy(physical, px, py);
    };
    checkBlock(8, 16, 8, 16);
    checkBlock(40, 200, 16, 8);
    checkBlock(200, 400, 24, 24);

    // A 8x16 block becomes a 16x8 physical block: width/height swap + CW.
    // A's logical (8,16)..(15,31) -> phy (16,471)..(31,464)
    assert(phyIsBlack(physical, 16, 471));
    assert(phyIsBlack(physical, 31, 464));
    expectWhiteAtPhy(physical, 15, 471);
    expectWhiteAtPhy(physical, 16, 472);
  }

  // Horizontal + vertical stripes, including a non-byte-aligned vertical.
  {
    std::vector<uint8_t> logical = whiteLogical();
    // Horizontal logical row y=100 -> entire physical column x=100.
    for (int x = 0; x < 480; ++x) logSetBlack(logical, x, 100);
    // Vertical logical column x=17 (bit 6 of byte 2) -> entire physical row y=462.
    for (int y = 0; y < 800; ++y) logSetBlack(logical, 17, y);

    const std::vector<uint8_t> physical = mapOrDie(logical);
    // Intersection counted once: 480 + 800 - 1.
    assert(countBlack(physical) == 480 + 800 - 1);

    for (int py = 0; py < 480; ++py) {
      assert(phyIsBlack(physical, 100, py));
    }
    for (int px = 0; px < 800; ++px) {
      assert(phyIsBlack(physical, px, 462));
    }
    // Physical row 462 is a solid 0x00 row (800 black pixels).
    for (int i = 0; i < 100; ++i) {
      assert(physical[462 * 100 + i] == 0x00);
    }
    // Physical column 100 is bit 3 of every row (100 % 8 == 4 -> bit 7-4 = 3).
    const uint8_t colMask = static_cast<uint8_t>(0x80u >> (100 % 8));
    assert(colMask == 0x08);
    for (int py = 0; py < 480; ++py) {
      if (py == 462) continue;
      assert((physical[py * 100 + (100 / 8)] & colMask) == 0);
      // Adjacent bit in the same byte stays white.
      assert((physical[py * 100 + (100 / 8)] & static_cast<uint8_t>(colMask << 1)) != 0);
    }
  }

  // Edge pixels that cross byte boundaries on both planes.
  {
    std::vector<uint8_t> logical = whiteLogical();
    logSetBlack(logical, 7, 0);   // last bit of logical byte 0
    logSetBlack(logical, 8, 0);   // first bit of logical byte 1
    logSetBlack(logical, 0, 7);   // last bit of physical byte 0 after map
    logSetBlack(logical, 0, 8);   // first bit of physical byte 1 after map
    logSetBlack(logical, 7, 7);
    logSetBlack(logical, 8, 8);

    const std::vector<uint8_t> physical = mapOrDie(logical);
    assert(countBlack(physical) == 6);

    // (7,0)->(0,472) bit7 and (7,7)->(7,472) bit0 share byte 47200.
    assert(physical[472 * 100 + 0] == static_cast<uint8_t>(0x7E));
    // (8,0)  -> (0,471) byte 47100 bit7
    assert(physical[471 * 100 + 0] == static_cast<uint8_t>(0x7F));
    // (0,7)  -> (7,479) byte 47900 bit0
    assert(physical[479 * 100 + 0] == static_cast<uint8_t>(0xFE));
    // (0,8)  -> (8,479) byte 47901 bit7
    assert(physical[479 * 100 + 1] == static_cast<uint8_t>(0x7F));
    // (8,8)  -> (8,471) byte 47101 bit7
    assert(physical[471 * 100 + 1] == static_cast<uint8_t>(0x7F));

    expectBlackAt(physical, 7, 0);
    expectBlackAt(physical, 8, 0);
    expectBlackAt(physical, 0, 7);
    expectBlackAt(physical, 0, 8);
    expectBlackAt(physical, 7, 7);
    expectBlackAt(physical, 8, 8);
  }

  // Sparse deterministic random pattern: exact destinations + inverse identity.
  {
    std::vector<uint8_t> logical = whiteLogical();
    uint32_t seed = 0x4D335001u;
    const int n = 97;
    int placed = 0;
    while (placed < n) {
      const int x = static_cast<int>(lcg(seed) % 480u);
      const int y = static_cast<int>(lcg(seed) % 800u);
      if (!logIsBlack(logical, x, y)) {
        logSetBlack(logical, x, y);
        ++placed;
      }
    }
    assert(countBlack(logical) == n);

    const std::vector<uint8_t> physical = mapOrDie(logical);
    assert(countBlack(physical) == n);
    for (int y = 0; y < 800; ++y) {
      for (int x = 0; x < 480; ++x) {
        if (logIsBlack(logical, x, y)) expectBlackAt(physical, x, y);
      }
    }

    std::vector<uint8_t> back(48000, 0x00);
    size_t written = 0;
    assert(M4PanelMapper::mapPhysicalToLogical(physical.data(), physical.size(), back.data(),
                                               back.size(), &written) == Status::Ok);
    assert(written == 48000u);
    assert(back == logical);
  }

  // Full-frame mixed pattern: every 3rd logical column + every 5th logical row.
  // Inverse must restore the original; absolute sample points must match.
  {
    std::vector<uint8_t> logical = whiteLogical();
    for (int y = 0; y < 800; ++y) {
      for (int x = 0; x < 480; ++x) {
        if ((x % 3) == 0 || (y % 5) == 0) logSetBlack(logical, x, y);
      }
    }
    const std::vector<uint8_t> physical = mapOrDie(logical);
    assert(countBlack(physical) == countBlack(logical));
    expectBlackAt(physical, 0, 0);
    expectBlackAt(physical, 3, 17);
    expectBlackAt(physical, 17, 5);
    expectBlackAt(physical, 477, 795);
    expectBlackAt(physical, 0, 799);
    expectBlackAt(physical, 479, 0);
    int px = 0, py = 0;
    expectedPhy(1, 1, &px, &py);
    expectWhiteAtPhy(physical, px, py);

    std::vector<uint8_t> back(48000, 0x00);
    assert(M4PanelMapper::mapPhysicalToLogical(physical.data(), physical.size(), back.data(),
                                               back.size(), nullptr) == Status::Ok);
    assert(back == logical);
  }

  std::printf("test_m4_panel_mapper: PASS\n");
  return 0;
}

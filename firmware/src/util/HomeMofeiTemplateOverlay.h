#pragma once

// Mofei Home black-ink overlay — M4TH v1 ASSET_DATA section.
// Correct order per template: A) clear white, B) draw dynamic covers/text, C) overlay ink LAST.
// Bit 1 = draw black, 0 = transparent/no-op. Never paint white over dynamic.
// Compatibility wrapper over generic UiScene::GfxSceneRenderer — preserves M4TH v1 ordered bitmap semantics.

#include <cstdint>
#include <cstddef>
#include <GfxRenderer.h>
#include "ui/scene/GfxSceneRenderer.h"

#ifdef __AVR__
#include <avr/pgmspace.h>
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif
#else
// Host/simulator — PROGMEM is no-op, pgm_read_byte is direct.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif
#endif

namespace HomeMofeiTemplateOverlay {

// M4TH v1 constants (must match compile_home_theme.py)
static constexpr uint32_t kMagic = 0x4854344D; // 'M4TH' little-endian
static constexpr uint16_t kVersion = 1;
static constexpr uint16_t kHeaderSize = 32;
static constexpr uint16_t kScreenW = 480;
static constexpr uint16_t kScreenH = 800;
static constexpr uint16_t kSectionCount = 5;
static constexpr size_t kStride = 60;
static constexpr size_t kAssetDataLen = 48000;

inline bool validateHeader(const uint8_t* data, size_t len) {
  if (!data || len < 32) return false;
  uint32_t magic = (uint32_t)pgm_read_byte(data+0) | ((uint32_t)pgm_read_byte(data+1)<<8) |
                   ((uint32_t)pgm_read_byte(data+2)<<16) | ((uint32_t)pgm_read_byte(data+3)<<24);
  if (magic != kMagic) return false;
  uint16_t ver = pgm_read_byte(data+4) | (pgm_read_byte(data+5)<<8);
  uint16_t hs = pgm_read_byte(data+6) | (pgm_read_byte(data+7)<<8);
  if (ver != kVersion || hs != kHeaderSize) return false;
  uint32_t total = pgm_read_byte(data+8) | (pgm_read_byte(data+9)<<8) | (pgm_read_byte(data+10)<<16) | (pgm_read_byte(data+11)<<24);
  if (total != len) return false;
  uint16_t sw = pgm_read_byte(data+12) | (pgm_read_byte(data+13)<<8);
  uint16_t sh = pgm_read_byte(data+14) | (pgm_read_byte(data+15)<<8);
  if (sw != kScreenW || sh != kScreenH) return false;
  return true;
}

// Find ASSET_DATA offset/len by parsing section table. Returns false if not found.
inline bool findAssetData(const uint8_t* data, size_t len, size_t &outOffset, size_t &outLen) {
  if (!validateHeader(data, len)) return false;
  // Section table at 32, 5 * 24 bytes
  for (int i = 0; i < 5; ++i) {
    size_t off = 32 + i*24;
    uint32_t typ = pgm_read_byte(data+off) | (pgm_read_byte(data+off+1)<<8) | (pgm_read_byte(data+off+2)<<16) | (pgm_read_byte(data+off+3)<<24);
    uint32_t secOff = pgm_read_byte(data+off+8) | (pgm_read_byte(data+off+9)<<8) | (pgm_read_byte(data+off+10)<<16) | (pgm_read_byte(data+off+11)<<24);
    uint32_t secLen = pgm_read_byte(data+off+12) | (pgm_read_byte(data+off+13)<<8) | (pgm_read_byte(data+off+14)<<16) | (pgm_read_byte(data+off+15)<<24);
    // type 5 = ASSET_DATA
    if (typ == 5) {
      if (secOff + secLen > len) return false;
      outOffset = secOff;
      outLen = secLen;
      return true;
    }
  }
  return false;
}

// Draw the 1-bit black ink overlay. Bit 1 = draw black pixel, 0 = no-op (transparent).
// Works on device PROGMEM and host simulator.
// Delegates to generic GfxSceneRenderer::drawPackageAssetData to preserve black-ink/no-op-white and ordered bitmap semantics.
inline void draw(const GfxRenderer& renderer, const uint8_t* data, size_t len) {
  // Generic path handles section lookup, stride, MSB-first, and pgm_read_byte correctly.
  UiScene::GfxSceneRenderer::drawPackageAssetData(renderer, data, len);
}

// Convenience for the generated mofei pack.
inline void drawMofeiClassic(const GfxRenderer& renderer) {
  extern const uint8_t mofei_classic_m4theme[] PROGMEM;
  extern const uint32_t mofei_classic_m4theme_len;
  // The generated header defines these; include it where used.
  // This wrapper avoids hardcoding offset — it parses the pack.
  draw(renderer, mofei_classic_m4theme, mofei_classic_m4theme_len);
}

} // namespace HomeMofeiTemplateOverlay

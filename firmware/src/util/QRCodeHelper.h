#pragma once

#include <GfxRenderer.h>
#include <qrcode.h>

#include <string>

/**
 * Shared QR code rendering utility.
 * Renders a Version 4 QR code (33 modules) with ECC_LOW on an e-ink display.
 */
namespace QRCodeHelper {

/// Version 4 QR code = 33 modules per side
constexpr uint8_t QR_MODULES = 33;

/// Default pixels per QR module (same as File Transfer)
constexpr uint8_t DEFAULT_PX = 6;

/**
 * Draw a QR code on the display, auto-selecting the version so the payload
 * fits (ECC_LOW capacities: V4=78B ... V10=271B). The ricmoo qrcode library
 * does NOT reject oversized input — it silently writes past the module buffer
 * and produces an undecodable code (jjwxc login URL is 98B, needs V5). Check
 * the init return and bail out (return false) instead of painting garbage.
 * @param renderer  The GfxRenderer to draw on
 * @param x         Top-left X coordinate
 * @param y         Top-left Y coordinate
 * @param data      The data to encode in the QR code
 * @param px        Pixels per QR module (default: 6)
 * @return          true when a valid code was drawn
 */
inline bool drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data,
                       const uint8_t px = DEFAULT_PX) {
  // ECC_LOW byte capacity per version V4..V10.
  static const uint16_t kCapBytes[7] = {78, 106, 134, 154, 192, 230, 271};
  const size_t len = data.size();
  int version = 4;
  for (int i = 0; i < 7; ++i) {
    version = 4 + i;
    if (len <= kCapBytes[i]) break;
  }
  if (len > 271) return false;  // beyond V10 — refuse, do not paint garbage

  QRCode qrcode;
  uint8_t qrcodeBytes[512];  // room for V10 (57x57 grid)
  if (qrcode_initText(&qrcode, qrcodeBytes, static_cast<uint8_t>(version), ECC_LOW, data.c_str()) != 0) {
    return false;
  }
  for (uint8_t cy = 0; cy < qrcode.size; cy++) {
    for (uint8_t cx = 0; cx < qrcode.size; cx++) {
      if (qrcode_getModule(&qrcode, cx, cy)) {
        renderer.fillRect(x + px * cx, y + px * cy, px, px, true);
      }
    }
  }
  return true;
}

/**
 * Calculate the total pixel size of a QR code.
 * @param px  Pixels per module
 * @return    Total size in pixels (width = height)
 */
constexpr int qrSize(const uint8_t px = DEFAULT_PX) { return px * QR_MODULES; }

}  // namespace QRCodeHelper
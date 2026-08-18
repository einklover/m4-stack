#include "GfxRenderer.h"

#include <Utf8.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef CROSSPOINT_MURPHY_M4
#include <esp_heap_caps.h>

#include "../../src/CrossPointSettings.h"
#include "../../src/debug/M4WaveformLab.h"
#include "../../src/util/M4DisplayOwner.h"
#endif

// Half-buffer geometry constants derived from display dimensions (orientation-aware).
// Portrait: logical top half -> physical LEFT half (bytes 0..HALF_ROW_BYTES-1 per row)
// Landscape: logical top half -> physical rows 0..HALF_ROWS-1
static constexpr size_t HALF_ROW_BYTES     = HalDisplay::DISPLAY_WIDTH / 2 / 8;
static constexpr size_t HALF_ROWS          = HalDisplay::DISPLAY_HEIGHT / 2;
static constexpr size_t HALF_BUF_PORTRAIT  = HALF_ROW_BYTES * HalDisplay::DISPLAY_HEIGHT;
static constexpr size_t HALF_BUF_LANDSCAPE = HALF_ROWS * HalDisplay::DISPLAY_WIDTH_BYTES;
static constexpr size_t HALF_BUF_SIZE = HALF_BUF_LANDSCAPE > HALF_BUF_PORTRAIT
                                        ? HALF_BUF_LANDSCAPE : HALF_BUF_PORTRAIT;

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer\n", millis());
    assert(false);
  }
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) { fontMap.insert({fontId, font}); }

void GfxRenderer::replaceFont(const int fontId, EpdFontFamily font) { fontMap[fontId] = font; }

bool GfxRenderer::hasFont(const int fontId) const { return fontMap.count(fontId) > 0; }

bool GfxRenderer::hasTextGlyphs(const int fontId, const char* text,
                                const EpdFontFamily::Style style) const {
  if (!text) return true;
  const auto it = fontMap.find(fontId);
  if (it == fontMap.end()) return false;
  const EpdFontFamily& family = it->second;
  const EpdGlyph* question = family.getGlyph('?', style);
  EpdGlyph questionCopy{};
  if (question) questionCopy = *question;
  const char* p = text;
  uint32_t cp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p))) != 0) {
    if (cp <= 0x20 || cp == '?') continue;
    const EpdGlyph* glyph = family.getGlyph(cp, style);
    if (!glyph || glyph->width == 0 || glyph->height == 0) return false;
    // SD-backed font implementations intentionally return the question glyph
    // for misses. Compare a copy because their glyph object is a shared cache.
    if (question && glyph->width == questionCopy.width && glyph->height == questionCopy.height &&
        glyph->advanceX == questionCopy.advanceX && glyph->left == questionCopy.left &&
        glyph->top == questionCopy.top && glyph->dataLength == questionCopy.dataLength &&
        glyph->dataOffset == questionCopy.dataOffset) {
      return false;
    }
  }
  return true;
}

void GfxRenderer::clearCustomFonts(const int startId) {
  for (auto it = fontMap.lower_bound(startId); it != fontMap.end();) {
    it = fontMap.erase(it);
  }
}
int GfxRenderer::getTextAdvance(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  return fontMap.at(fontId).getTextAdvance(text, style);
}
// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - x;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

int GfxRenderer::logicalToPhysicalAnimationDirection(const int logicalDirection) const {
  if (logicalDirection < 0 || logicalDirection > 3) return 0;
  // Directions are encoded as 0=R→L, 1=L→R, 2=B→T, 3=T→B.  The panel
  // framebuffer is always landscape; this table is the same coordinate
  // transform used by rotateCoordinates(), applied to a sweep vector.
  static constexpr int kMap[4][4] = {
      /* Portrait                */ {3, 2, 0, 1},
      /* LandscapeClockwise      */ {1, 0, 3, 2},
      /* PortraitInverted        */ {2, 3, 1, 0},
      /* LandscapeCounterClockwise */ {0, 1, 2, 3},
  };
  return kMap[static_cast<int>(orientation)][logicalDirection];
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY);

  // Bounds checking against physical panel dimensions
  if (phyX < 0 || phyX >= HalDisplay::DISPLAY_WIDTH || phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) {
    // Serial.printf("[%lu] [GFX] !! Outside range (%d, %d) -> (%d, %d)\n", millis(), x, y, phyX, phyY);
    return;
  }

  // Calculate byte position and bit position
  const uint16_t byteIndex = phyY * HalDisplay::DISPLAY_WIDTH_BYTES + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const float scale) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  // Advance, not ink bbox. Runtime TTF wrapping/index must not rasterize.
  const int w = fontMap.at(fontId).getTextAdvance(text, style);
  if (scale == 1.0f || scale <= 0.0f) return w;
  return static_cast<int>(w * scale + 0.5f);
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const float scale) const {
  const float s = (scale > 0.0f) ? scale : 1.0f;
  const int yPos = y + static_cast<int>(getFontAscenderSize(fontId) * s + 0.5f);
  int xpos = x;

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return;
  }
  const auto font = fontMap.at(fontId);

  // no printable characters
  if (!font.hasPrintableChars(text, style)) {
    return;
  }

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    renderChar(font, cp, &xpos, &yPos, black, style, s);
  }
}

float GfxRenderer::scaleFontToMatch(const int srcFontId, const int targetFontId) const {
  const int srcAsc = getFontAscenderSize(srcFontId);
  const int tgtAsc = getFontAscenderSize(targetFontId);
  if (srcAsc <= 0 || tgtAsc <= 0) return 1.0f;
  // Prefer slight shrink so body epdfont (~16px face) fits UI-13 metrics.
  float s = static_cast<float>(tgtAsc) / static_cast<float>(srcAsc);
  if (s > 1.0f) s = 1.0f;   // never upscale (blurry + layout blow-up)
  if (s < 0.55f) s = 0.55f; // avoid unreadable ultra-small
  // If already matched (same face), skip scaling work.
  if (s > 0.97f) return 1.0f;
  return s;
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // TODO: Implement
    Serial.printf("[%lu] [GFX] Line drawing not supported\n", millis());
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - i, y + i, state);
    drawLine(x + width - i, y + i, x + width - i, y + height - i, state);
    drawLine(x + width - i, y + height - i, x + i, y + height - i, state);
    drawLine(x + i, y + height - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadiusSq = maxRadius * maxRadius;
  const int innerRadiusSq = innerRadius * innerRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    for (int dx = 0; dx <= maxRadius; ++dx) {
      const int distSq = dx * dx + dy * dy;
      if (distSq > outerRadiusSq || distSq < innerRadiusSq) {
        continue;
      }
      const int px = cx + xDir * dx;
      const int py = cy + yDir * dy;
      drawPixel(px, py, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  if (color == Color::Clear) {
  } else if (color == Color::Black) {
    fillRect(x, y, width, height, true);
  } else if (color == Color::White) {
    fillRect(x, y, width, height, false);
  } else if (color == Color::LightGray) {
    for (int fillY = y; fillY < y + height; fillY++) {
      for (int fillX = x; fillX < x + width; fillX++) {
        drawPixelDither<Color::LightGray>(fillX, fillY);
      }
    }
  } else if (color == Color::DarkGray) {
    for (int fillY = y; fillY < y + height; fillY++) {
      for (int fillX = x; fillX < x + width; fillX++) {
        drawPixelDither<Color::DarkGray>(fillX, fillY);
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  const int radiusSq = maxRadius * maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    for (int dx = 0; dx <= maxRadius; ++dx) {
      const int distSq = dx * dx + dy * dy;
      const int px = cx + xDir * dx;
      const int py = cy + yDir * dy;
      if (distSq <= radiusSq) {
        drawPixelDither<color>(px, py);
      }
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int verticalHeight = height - 2 * maxRadius - 2;
  if (verticalHeight > 0) {
    fillRectDither(x, y + maxRadius + 1, maxRadius + 1, verticalHeight, color);
    fillRectDither(x + width - maxRadius - 1, y + maxRadius + 1, maxRadius + 1, verticalHeight, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  } else {
    fillRectDither(x, y, maxRadius + 1, maxRadius + 1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  } else {
    fillRectDither(x + width - maxRadius - 1, y, maxRadius + 1, maxRadius + 1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  } else {
    fillRectDither(x + width - maxRadius - 1, y + height - maxRadius - 1, maxRadius + 1, maxRadius + 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  } else {
    fillRectDither(x, y + height - maxRadius - 1, maxRadius + 1, maxRadius + 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  display.drawImageTransparent(bitmap, y, getScreenWidth() - width - x, height, width);
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  Serial.printf("[%lu] [GFX] Cropping %dx%d by %dx%d pix, is %s\n", millis(), bitmap.getWidth(), bitmap.getHeight(),
                cropPixX, cropPixY, bitmap.isTopDown() ? "top-down" : "bottom-up");

  if (maxWidth > 0 && (1.0f - cropX) * bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>((1.0f - cropX) * bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && (1.0f - cropY) * bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>((1.0f - cropY) * bitmap.getHeight()));
    isScaled = true;
  }
  Serial.printf("[%lu] [GFX] Scaling by %f - %s\n", millis(), scale, isScaled ? "scaled" : "not scaled");

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    Serial.printf("[%lu] [GFX] !! Failed to allocate BMP row buffers\n", millis());
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      Serial.printf("[%lu] [GFX] Failed to read row %d from bitmap\n", millis(), bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }

  free(outputRow);
  free(rowBytes);
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    Serial.printf("[%lu] [GFX] !! Failed to allocate 1-bit BMP row buffers\n", millis());
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      Serial.printf("[%lu] [GFX] Failed to read row %d from 1-bit bitmap\n", millis(), bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }

  free(outputRow);
  free(rowBytes);
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    Serial.printf("[%lu] [GFX] !! Failed to allocate polygon node buffer\n", millis());
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X (simple bubble sort, numPoints is small)
    for (int i = 0; i < nodes - 1; i++) {
      for (int k = i + 1; k < nodes; k++) {
        if (nodeX[i] > nodeX[k]) {
          int temp = nodeX[i];
          nodeX[i] = nodeX[k];
          nodeX[k] = temp;
        }
      }
    }

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
#ifdef CROSSPOINT_MURPHY_M4
  if (m4BrowserBridgeIsPresenting()) {
    Serial.printf("[%lu] [GFX] skip clearScreen — browser present in flight\n", millis());
    return;
  }
#endif
  start_ms = millis();
  display.clearScreen(color);
}

void GfxRenderer::invertScreen() const {
  for (int i = 0; i < HalDisplay::BUFFER_SIZE; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::armEntryAnimation(const int direction) {
#ifdef CROSSPOINT_MURPHY_M4
  cancelEntryAnimation();
  const uint8_t* source = frameBuffer ? frameBuffer : lastShownFrame;
  if (!source) return;

  pendingEntryFrame = static_cast<uint8_t*>(
      heap_caps_malloc(HalDisplay::BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pendingEntryFrame) return;
  std::memcpy(pendingEntryFrame, source, HalDisplay::BUFFER_SIZE);
  pendingEntryDirection = (direction >= 0 && direction <= 3) ? direction : 0;
  // Back/Home can enter a new activity whose first paint waits on a worker.
  // Keep the source alive long enough for that paint, but never let it leak
  // into a later unrelated navigation action.
  pendingEntryDeadlineMs = millis() + 1500;
#else
  (void)direction;
#endif
}

void GfxRenderer::cancelEntryAnimation() {
  pendingEntryDeadlineMs = 0;
  if (pendingEntryFrame) {
    free(pendingEntryFrame);
    pendingEntryFrame = nullptr;
  }
}

void GfxRenderer::ageEntryAnimation() {
#ifdef CROSSPOINT_MURPHY_M4
  if (pendingEntryFrame && pendingEntryDeadlineMs != 0 &&
      static_cast<int32_t>(millis() - pendingEntryDeadlineMs) >= 0) {
    cancelEntryAnimation();
  }
#endif
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
#ifdef CROSSPOINT_MURPHY_M4
  if (m4BrowserBridgeOwnsDisplay()) {
    Serial.printf("[%lu] [GFX] skip displayBuffer — browser bridge owns panel\n", millis());
    return;
  }
#endif
  auto elapsed = millis() - start_ms;
  Serial.printf("[%lu] [GFX] Time = %lu ms from clearScreen to displayBuffer\n", millis(), elapsed);

#ifdef CROSSPOINT_MURPHY_M4
  if (pendingEntryFrame && frameBuffer && SETTINGS.systemAnimationEnabled != 0) {
    uint8_t* oldFrame = pendingEntryFrame;
    pendingEntryFrame = nullptr;
    pendingEntryDeadlineMs = 0;

    int steps = static_cast<int>(SETTINGS.pageTurnAnimationSteps);
    if (steps < 2) steps = 2;
    if (steps > 64) steps = 64;
    int mult = static_cast<int>(SETTINGS.pageTurnAnimationMult);
    if (mult < 1) mult = 1;
    if (mult > 16) mult = 16;
    if (mult > steps) mult = steps;
    uint8_t tp = SETTINGS.pageTurnAnimationTp;
    if (tp < 1) tp = 1;
    if (tp > 16) tp = 16;
    uint8_t fr = SETTINGS.pageTurnAnimationFrameRate;
    if (fr != 0x22 && fr != 0x44 && fr != 0x88) fr = 0x88;

    uint8_t lut[M4WaveformLab::kLutBytes] = {};
    lut[10] = 0x80;
    lut[20] = 0x40;
    lut[50] = tp;
    lut[51] = 0x01;
    for (int i = 100; i < 105; ++i) lut[i] = fr;
    lut[105] = 0x17;
    lut[106] = 0x41;
    lut[107] = 0xA8;
    lut[108] = 0x32;
    lut[109] = 0x30;

    M4WaveformLab::setDisplay(&display);
    const bool played = M4WaveformLab::setLutBytes(lut, sizeof(lut)) &&
                        M4WaveformLab::runAnimateMemWindow(
                            oldFrame, frameBuffer, steps, mult,
                            logicalToPhysicalAnimationDirection(pendingEntryDirection)) != 0;
    free(oldFrame);
    if (played) {
      (void)const_cast<GfxRenderer*>(this)->storeLastShown();
      m4BrowserBridgeInvalidatePhysicalBaseline();
      return;
    }
  } else {
    const_cast<GfxRenderer*>(this)->cancelEntryAnimation();
  }
#endif

  display.displayBuffer(refreshMode, fadingFix);
#ifdef CROSSPOINT_MURPHY_M4
  m4BrowserBridgeInvalidatePhysicalBaseline();
#endif
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style, const float scale) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  const char* ellipsis = "...";
  int textWidth = getTextWidth(fontId, item.c_str(), style, scale);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style, scale) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
void GfxRenderer::tapToLogical(float nx, float ny, int& outX, int& outY) const {
  // Physical panel is always landscape DISPLAY_WIDTH x DISPLAY_HEIGHT (800x480).
  const int panelWidth = HalDisplay::DISPLAY_WIDTH;
  const int panelHeight = HalDisplay::DISPLAY_HEIGHT;
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;

  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return HalDisplay::DISPLAY_HEIGHT;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return HalDisplay::DISPLAY_WIDTH;
  }
  return HalDisplay::DISPLAY_HEIGHT;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return HalDisplay::DISPLAY_WIDTH;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return HalDisplay::DISPLAY_HEIGHT;
  }
  return HalDisplay::DISPLAY_WIDTH;
}

int GfxRenderer::getSpaceWidth(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

    const EpdGlyph* glyph = fontMap.at(fontId).getGlyph(' ', EpdFontFamily::REGULAR);
  if (!glyph) {
    // Serial.printf("[%lu] [GFX] Font %d (Regular) has no space glyph! Using fallback.\n", millis(), fontId);
    const EpdFontData* data = fontMap.at(fontId).getData(EpdFontFamily::REGULAR);
    if (data) {
      return data->ascender / 3;
    }
    return 0;
  }
  return glyph->advanceX;
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  uint32_t cp;
  int width = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    width += fontMap.at(fontId).glyphAdvanceX(cp, EpdFontFamily::REGULAR);
  }
  return width;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

    const EpdFontData* data = fontMap.at(fontId).getData(EpdFontFamily::REGULAR);
  if (!data) {
    Serial.printf("[%lu] [GFX] Font %d (Regular) has no data!\n", millis(), fontId);
    return 0;
  }

  return data->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  const EpdFontData* data = fontMap.at(fontId).getData(EpdFontFamily::REGULAR);
  if (!data) {
    Serial.printf("[%lu] [GFX] Font %d (Regular) has no data!\n", millis(), fontId);
    return 0;
  }

  return data->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }
  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return;
  }
  const auto font = fontMap.at(fontId);

  // No printable characters
  if (!font.hasPrintableChars(text, style)) {
    return;
  }

  // For 90° clockwise rotation:
  // Original (glyphX, glyphY) -> Rotated (glyphY, -glyphX)
  // Text reads from bottom to top

  int yPos = y;  // Current Y position (decreases as we draw characters)

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) {
      glyph = font.getGlyph('?', style);
    }
    if (!glyph) {
      continue;
    }

    const int is2Bit = font.getData(style)->is2Bit;
    const uint32_t offset = glyph->dataOffset;
    const uint8_t width = glyph->width;
    const uint8_t height = glyph->height;
    const int left = glyph->left;
    const int top = glyph->top;

    // Use loadGlyphBitmap to support both static and custom (SD-based) fonts
    uint8_t* buffer = nullptr;  // Not used for now, as we expect a pointer or cache
    const uint8_t* bitmap = font.loadGlyphBitmap(glyph, buffer, style);

    if (bitmap != nullptr) {
      for (int glyphY = 0; glyphY < height; glyphY++) {
        for (int glyphX = 0; glyphX < width; glyphX++) {
          const int pixelPosition = glyphY * width + glyphX;

          // 90° clockwise rotation transformation:
          // screenX = x + (ascender - top + glyphY)
          // screenY = yPos - (left + glyphX)
          const int screenX = x + (font.getData(style)->ascender - top + glyphY);
          const int screenY = yPos - left - glyphX;

          if (is2Bit) {
            const uint8_t byte = bitmap[pixelPosition / 4];
            const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
            const uint8_t bmpVal = 3 - (byte >> bit_index) & 0x3;

            if (renderMode == BW && bmpVal < 3) {
              drawPixel(screenX, screenY, black);
            } else if (renderMode == GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
              drawPixel(screenX, screenY, false);
            } else if (renderMode == GRAYSCALE_LSB && bmpVal == 1) {
              drawPixel(screenX, screenY, false);
            }
          } else {
            const uint8_t byte = bitmap[pixelPosition / 8];
            const uint8_t bit_index = 7 - (pixelPosition % 8);

            if ((byte >> bit_index) & 1) {
              drawPixel(screenX, screenY, black);
            }
          }
        }
      }
    }

    // Move to next character position (going up, so decrease Y)
    yPos -= glyph->advanceX;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

bool GfxRenderer::storeLastShown() {
  if (!frameBuffer) return false;
  if (!lastShownFrame) {
    lastShownFrame = static_cast<uint8_t*>(
        heap_caps_malloc(HalDisplay::BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!lastShownFrame) return false;
  }
  std::memcpy(lastShownFrame, frameBuffer, HalDisplay::BUFFER_SIZE);
  return true;
}

size_t GfxRenderer::getBufferSize() { return HalDisplay::BUFFER_SIZE; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer() const { display.displayGrayBuffer(fadingFix); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      Serial.printf("[%lu] [GFX] !! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk\n",
                    millis(), i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(BW_BUFFER_CHUNK_SIZE));

    if (!bwBufferChunks[i]) {
      Serial.printf("[%lu] [GFX] !! Failed to allocate BW buffer chunk %zu (%zu bytes)\n", millis(), i,
                    BW_BUFFER_CHUNK_SIZE);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, BW_BUFFER_CHUNK_SIZE);
  }

  Serial.printf("[%lu] [GFX] Stored BW buffer in %zu chunks (%zu bytes each)\n", millis(), BW_BUFFER_NUM_CHUNKS,
                BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if any all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if chunk is missing
    if (!bwBufferChunks[i]) {
      Serial.printf("[%lu] [GFX] !! BW buffer chunks not stored - this is likely a bug\n", millis());
      freeBwBufferChunks();
      return;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    memcpy(frameBuffer + offset, bwBufferChunks[i], BW_BUFFER_CHUNK_SIZE);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  Serial.printf("[%lu] [GFX] Restored and freed BW buffer chunks\n", millis());
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

/**
 * Saves the logical "top half" of the current framebuffer into an internal rolling buffer
 * (24KB). This is used by the rolling auto-turn feature to blend two pages together:
 *   1. Render page N+1 → saveTopHalfBuffer() → save N+1 top half
 *   2. Render page N   → restoreTopHalfToFrame() → paste N+1 top over N top
 * Result: top half shows start of next page, bottom half shows end of current page.
 *
 * Memory strategy: reuse the existing buffer if already allocated.  This avoids
 * repeated malloc/free cycles within a single rolling half-turn, which is important
 * when memory is tight (e.g., after storeBwBuffer() consumes ~48KB).
 *
 * Physical byte mapping per orientation:
 *   Portrait            : bytes  0-49 of each of 480 physical rows (phyX 0..399)
 *   PortraitInverted    : bytes 50-99 of each of 480 physical rows (phyX 400..799)
 *   LandscapeClockwise  : full rows 240-479 (physical rows)
 *   LandscapeCounterCCW : full rows 0-239   (physical rows)
 * All cases save exactly BUFFER_SIZE/2 = 24000 bytes.
 */

/**
 * Pre-allocate the rolling half buffer without copying any content.
 * This should be called BEFORE storeBwBuffer() to avoid memory fragmentation.
 * storeBwBuffer() uses chunked allocation (6x8KB) which fragments the heap,
 * making it impossible to allocate a contiguous 24KB block afterwards.
 */
bool GfxRenderer::preAllocateTopHalfBuffer() {
  if (rollingHalfBuffer) {
    return true;  // Already allocated
  }
  rollingHalfBuffer = static_cast<uint8_t*>(malloc(HALF_BUF_SIZE));
  if (!rollingHalfBuffer) {
    Serial.printf("[%lu] [GFX] !! Failed to pre-alloc rolling half buffer\n", millis());
    return false;
  }
  Serial.printf("[%lu] [GFX] Pre-allocated rolling half buffer (24KB)\n", millis());
  return true;
}

bool GfxRenderer::saveTopHalfBuffer() {
  // Reuse existing buffer if already allocated; only allocate if nullptr.
  // This allows multiple save/restore cycles within a single rolling half-turn
  // without requiring additional malloc when memory is tight.
  if (!rollingHalfBuffer) {
    rollingHalfBuffer = static_cast<uint8_t*>(malloc(HALF_BUF_SIZE));
    if (!rollingHalfBuffer) {
      Serial.printf("[%lu] [GFX] !! Failed to alloc rolling half buffer\n", millis());
      return false;
    }
  }

  const size_t rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  const uint8_t* fb = frameBuffer;

  switch (orientation) {
    case Portrait:
      // logical top half → physical columns 0..HALF_ROW_BYTES*8-1 (bytes 0..HALF_ROW_BYTES-1 per row)
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memcpy(rollingHalfBuffer + row * HALF_ROW_BYTES, fb + row * rowBytes, HALF_ROW_BYTES);
      }
      break;
    case PortraitInverted:
      // logical top half → physical columns HALF_ROW_BYTES*8..DISPLAY_WIDTH-1 (offset HALF_ROW_BYTES per row)
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memcpy(rollingHalfBuffer + row * HALF_ROW_BYTES, fb + row * rowBytes + HALF_ROW_BYTES, HALF_ROW_BYTES);
      }
      break;
    case LandscapeClockwise:
      // logical top half → physical rows HALF_ROWS..DISPLAY_HEIGHT-1
      memcpy(rollingHalfBuffer, fb + HALF_ROWS * rowBytes, HALF_ROWS * rowBytes);
      break;
    case LandscapeCounterClockwise:
      // logical top half → physical rows 0..HALF_ROWS-1
      memcpy(rollingHalfBuffer, fb, HALF_ROWS * rowBytes);
      break;
  }
  return true;
}

/**
 * Writes the saved top half back into the current framebuffer, replacing whatever
 * is currently in that region. Must be preceded by a successful saveTopHalfBuffer().
 */
void GfxRenderer::restoreTopHalfToFrame() {
  if (!rollingHalfBuffer) return;

  const size_t rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;

  switch (orientation) {
    case Portrait:
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memcpy(frameBuffer + row * rowBytes, rollingHalfBuffer + row * HALF_ROW_BYTES, HALF_ROW_BYTES);
      }
      break;
    case PortraitInverted:
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memcpy(frameBuffer + row * rowBytes + HALF_ROW_BYTES, rollingHalfBuffer + row * HALF_ROW_BYTES, HALF_ROW_BYTES);
      }
      break;
    case LandscapeClockwise:
      memcpy(frameBuffer + HALF_ROWS * rowBytes, rollingHalfBuffer, HALF_ROWS * rowBytes);
      break;
    case LandscapeCounterClockwise:
      memcpy(frameBuffer, rollingHalfBuffer, HALF_ROWS * rowBytes);
      break;
  }
}

void GfxRenderer::freeTopHalfBuffer() {
  if (rollingHalfBuffer) {
    free(rollingHalfBuffer);
    rollingHalfBuffer = nullptr;
  }
}

/**
 * SD card version of saveTopHalfBuffer.
 * Saves the logical top half of the framebuffer directly to an SD card file,
 * avoiding the need to allocate 24KB of RAM. This is useful when memory is tight.
 *
 * @param path Full path to the temporary file on SD card (e.g., "/cache/halfbuf.bin")
 * @return true if save succeeded, false on error
 */
bool GfxRenderer::saveTopHalfToSd(const char* path) {
  FsFile file;
  if (!SdMan.openFileForWrite("GFX", path, file)) {
    Serial.printf("[%lu] [GFX] !! Failed to open SD file for half buffer write: %s\n", millis(), path);
    return false;
  }

  const size_t rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  const uint8_t* fb = frameBuffer;
  const size_t halfSize = (orientation == Portrait || orientation == PortraitInverted)
                          ? HALF_BUF_PORTRAIT : HALF_BUF_LANDSCAPE;
  size_t written = 0;

  switch (orientation) {
    case Portrait:
      // logical top half → bytes 0..HALF_ROW_BYTES-1 per row
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        written += file.write(fb + row * rowBytes, HALF_ROW_BYTES);
      }
      break;
    case PortraitInverted:
      // logical top half → bytes HALF_ROW_BYTES..rowBytes-1 per row
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        written += file.write(fb + row * rowBytes + HALF_ROW_BYTES, HALF_ROW_BYTES);
      }
      break;
    case LandscapeClockwise:
      // logical top half → physical rows HALF_ROWS..DISPLAY_HEIGHT-1
      written = file.write(fb + HALF_ROWS * rowBytes, HALF_ROWS * rowBytes);
      break;
    case LandscapeCounterClockwise:
      // logical top half → physical rows 0..HALF_ROWS-1
      written = file.write(fb, HALF_ROWS * rowBytes);
      break;
  }

  file.close();

  if (written != halfSize) {
    Serial.printf("[%lu] [GFX] !! SD half buffer write incomplete: %zu/%zu bytes\n", millis(), written, halfSize);
    return false;
  }

  Serial.printf("[%lu] [GFX] Saved top half to SD (%zu bytes)\n", millis(), written);
  return true;
}

/**
 * SD card version of restoreTopHalfToFrame.
 * Reads the saved top half from SD card file directly into the framebuffer.
 *
 * @param path Full path to the temporary file on SD card
 * @return true if restore succeeded, false on error
 */
bool GfxRenderer::restoreTopHalfFromSd(const char* path) {
  FsFile file;
  if (!SdMan.openFileForRead("GFX", path, file)) {
    Serial.printf("[%lu] [GFX] !! Failed to open SD file for half buffer read: %s\n", millis(), path);
    return false;
  }

  const size_t rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  const size_t halfSize = (orientation == Portrait || orientation == PortraitInverted)
                          ? HALF_BUF_PORTRAIT : HALF_BUF_LANDSCAPE;
  size_t readTotal = 0;

  switch (orientation) {
    case Portrait:
      // logical top half → bytes 0..HALF_ROW_BYTES-1 per row
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        readTotal += file.read(frameBuffer + row * rowBytes, HALF_ROW_BYTES);
      }
      break;
    case PortraitInverted:
      // logical top half → bytes HALF_ROW_BYTES..rowBytes-1 per row
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        readTotal += file.read(frameBuffer + row * rowBytes + HALF_ROW_BYTES, HALF_ROW_BYTES);
      }
      break;
    case LandscapeClockwise:
      // logical top half → physical rows HALF_ROWS..DISPLAY_HEIGHT-1
      readTotal = file.read(frameBuffer + HALF_ROWS * rowBytes, HALF_ROWS * rowBytes);
      break;
    case LandscapeCounterClockwise:
      // logical top half → physical rows 0..HALF_ROWS-1
      readTotal = file.read(frameBuffer, HALF_ROWS * rowBytes);
      break;
  }

  file.close();

  if (readTotal != halfSize) {
    Serial.printf("[%lu] [GFX] !! SD half buffer read incomplete: %zu/%zu bytes\n", millis(), readTotal, halfSize);
    return false;
  }

  Serial.printf("[%lu] [GFX] Restored top half from SD (%zu bytes)\n", millis(), readTotal);
  return true;
}

/**
 * Zeros the logical top half of the frameBuffer (orientation-aware).
 *
 * After rendering a page in GRAYSCALE_LSB / GRAYSCALE_MSB mode, call this before
 * copyGrayscale*Buffers() to ensure only the bottom half's gray data is written to
 * the display's RAM.  The zeroed pixels map to LUT entry "00" (no voltage change),
 * so the display keeps whatever the previous BW refresh left in that region.
 *
 * Physical mapping mirrors saveTopHalfBuffer():
 *   Portrait            : bytes  0-49 of each of 480 physical rows  (phyX 0..399)
 *   PortraitInverted    : bytes 50-99 of each of 480 physical rows  (phyX 400..799)
 *   LandscapeClockwise  : full rows 240-479 (physical rows)
 *   LandscapeCounterCCW : full rows   0-239 (physical rows)
 */
void GfxRenderer::clearLogicalTopHalf() {
  const size_t rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;  // 100
  switch (orientation) {
    case Portrait:
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memset(frameBuffer + row * rowBytes, 0x00, HALF_ROW_BYTES);
      }
      break;
    case PortraitInverted:
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memset(frameBuffer + row * rowBytes + HALF_ROW_BYTES, 0x00, HALF_ROW_BYTES);
      }
      break;
    case LandscapeClockwise:
      // logical top half -> physical rows HALF_ROWS..DISPLAY_HEIGHT-1
      memset(frameBuffer + HALF_ROWS * rowBytes, 0x00, HALF_ROWS * rowBytes);
      break;
    case LandscapeCounterClockwise:
      // logical top half -> physical rows 0..HALF_ROWS-1
      memset(frameBuffer, 0x00, HALF_ROWS * rowBytes);
      break;
  }
}

/**
 * Zeros the logical bottom half of the frameBuffer (orientation-aware).
 *
 * Use before copyGrayscale*Buffers() to keep only the top half's gray data,
 * letting the bottom half retain its BW-refresh state via LUT entry "00".
 */
void GfxRenderer::clearLogicalBottomHalf() {
  const size_t rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;  // 100
  switch (orientation) {
    case Portrait:
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memset(frameBuffer + row * rowBytes + HALF_ROW_BYTES, 0x00, HALF_ROW_BYTES);
      }
      break;
    case PortraitInverted:
      for (uint16_t row = 0; row < HalDisplay::DISPLAY_HEIGHT; row++) {
        memset(frameBuffer + row * rowBytes, 0x00, HALF_ROW_BYTES);
      }
      break;
    case LandscapeClockwise:
      // logical bottom half -> physical rows 0..HALF_ROWS-1
      memset(frameBuffer, 0x00, HALF_ROWS * rowBytes);
      break;
    case LandscapeCounterClockwise:
      // logical bottom half -> physical rows HALF_ROWS..DISPLAY_HEIGHT-1
      memset(frameBuffer + HALF_ROWS * rowBytes, 0x00, HALF_ROWS * rowBytes);
      break;
  }
}

void GfxRenderer::renderChar(const EpdFontFamily& fontFamily, const uint32_t cp, int* x, const int* y,
                             const bool pixelState, const EpdFontFamily::Style style, const float scale) const {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    glyph = fontFamily.getGlyph('?', style);
  }

  if (!glyph) {
    return;
  }

  const EpdFont* font = fontFamily.getFont(style);
  if (!font) return;
  const EpdFontData* data = font->getData(style);
  if (!data) return;

  const int is2Bit = data->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const float s = (scale > 0.0f) ? scale : 1.0f;
  // Only chapter-list style paths pass scale != 1. Keep the historical pixel loop
  // bit-identical when unscaled so reader AA (2-bit planes) is not regressed.
  const bool scaled = (s < 0.999f || s > 1.001f);

  const uint8_t* bitmap = font->loadGlyphBitmap(glyph, nullptr, style);

  if (bitmap != nullptr && !scaled) {
    for (int glyphY = 0; glyphY < height; glyphY++) {
      const int screenY = *y - glyph->top + glyphY;
      for (int glyphX = 0; glyphX < width; glyphX++) {
        const int pixelPosition = glyphY * width + glyphX;
        const int screenX = *x + left + glyphX;

        if (is2Bit) {
          const uint8_t byte = bitmap[pixelPosition / 4];
          const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
          // Keep original precedence: (3 - (byte >> bit_index)) & 0x3
          // (do NOT "fix" to 3-((byte>>bit_index)&0x3) — that washed out AA).
          const uint8_t bmpVal = 3 - (byte >> bit_index) & 0x3;

          if (renderMode == BW && bmpVal < 3) {
            drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            drawPixel(screenX, screenY, false);
          } else if (renderMode == GRAYSCALE_LSB && bmpVal == 1) {
            drawPixel(screenX, screenY, false);
          }
        } else {
          const uint8_t byte = bitmap[pixelPosition / 8];
          const uint8_t bit_index = 7 - (pixelPosition % 8);

          if ((byte >> bit_index) & 1) {
            drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
    *x += glyph->advanceX;
    return;
  }

  // Scaled path: chapter list (BW). Use correct 2-bit nibble extract; ink if not white.
  if (bitmap != nullptr && width > 0 && height > 0) {
    const int outW = std::max(1, static_cast<int>(width * s + 0.5f));
    const int outH = std::max(1, static_cast<int>(height * s + 0.5f));
    const int outLeft = static_cast<int>(left * s + (left >= 0 ? 0.5f : -0.5f));
    const int outTop = static_cast<int>(glyph->top * s + (glyph->top >= 0 ? 0.5f : -0.5f));

    // Box-filter: count ink samples in source block → fewer mosaic edges than pure NN.
    for (int dy = 0; dy < outH; dy++) {
      const int y0 = (dy * height) / outH;
      const int y1 = std::max(y0 + 1, ((dy + 1) * height) / outH);
      const int screenY = *y - outTop + dy;
      for (int dx = 0; dx < outW; dx++) {
        const int x0 = (dx * width) / outW;
        const int x1 = std::max(x0 + 1, ((dx + 1) * width) / outW);
        int ink = 0, tot = 0;
        for (int sy = y0; sy < y1 && sy < height; sy++) {
          for (int sx = x0; sx < x1 && sx < width; sx++) {
            ++tot;
            const int pixelPosition = sy * width + sx;
            if (is2Bit) {
              const uint8_t byte = bitmap[pixelPosition / 4];
              const uint8_t bit_index = static_cast<uint8_t>((3 - pixelPosition % 4) * 2);
              const uint8_t raw = static_cast<uint8_t>((byte >> bit_index) & 0x3);
              const uint8_t bmpVal = static_cast<uint8_t>(3 - raw);
              if (bmpVal < 3) ++ink;  // treat gray+black as ink for BW list
            } else {
              const uint8_t byte = bitmap[pixelPosition / 8];
              const uint8_t bit_index = static_cast<uint8_t>(7 - (pixelPosition % 8));
              if ((byte >> bit_index) & 1) ++ink;
            }
          }
        }
        // Threshold ~ half the block → solid strokes, less checkerboard mosaic.
        if (tot > 0 && ink * 2 >= tot) {
          drawPixel(*x + outLeft + dx, screenY, pixelState);
        }
      }
    }
  }

  *x += std::max(1, static_cast<int>(glyph->advanceX * s + 0.5f));
}



void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}
void GfxRenderer::drawPngFromTxtpng(const char* txtpng_file_path) const {
    // 固定txtpng分辨率：480×800，与屏幕物理分辨率匹配
    const int pngWidth = 480;
    const int pngHeight = 800;

    // 1. 打开txtpng文件（复用你的SdMan文件操作）
    FsFile txtpng_file;
    if (!SdMan.openFileForRead("GFD", txtpng_file_path, txtpng_file)) {
        Serial.printf("[%lu] [GFX] Failed to open txtpng: %s\n", millis(), txtpng_file_path);
        return;
    }

    // 2. 单行缓冲区：适配480个数值+空格，2048字节足够
    char line_buffer[2048];
    // pngY：txtpng的行号（对应原始y坐标 0~799）
    int pngY = 0;

    // 3. 逐行读取txtpng（一行对应一个y坐标）
    while (txtpng_file.available() && pngY < pngHeight) {
        // 读取一行并补结束符，避免乱码
        int line_len = txtpng_file.readBytesUntil('\n', line_buffer, sizeof(line_buffer) - 1);
        line_buffer[line_len] = '\0';

        // 计算屏幕实际Y坐标：绘制偏移y + txtpng原始y
        int screenY = pngY;
        // 超出屏幕高度，直接终止绘制
        if (screenY >= getScreenHeight()) {
            break;
        }

        // 4. 分割当前行的灰度值（一个数值对应一个x坐标）
        char* token = strtok(line_buffer, " ");
        // pngX：txtpng的列号（对应原始x坐标 0~479）
        int pngX = 0;

        while (token != nullptr && pngX < pngWidth) {
            // 计算屏幕实际X坐标：绘制偏移x + txtpng原始x
            int screenX =  pngX;
            // 超出屏幕宽度，跳过当前行剩余像素
            if (screenX >= getScreenWidth()) {
                break;
            }

            // 5. 解析灰度值：-1=透明（跳过），0~255=有效灰度
            int gray_255 = atoi(token);
            uint8_t val = 0; // 映射为4级灰阶值（0~3），对齐drawBitmap的val

            // 有效像素判断+灰度值映射（0=白，3=黑，1=浅灰，2=深灰）
            if (gray_255 != -1 && gray_255 >= 0 && gray_255 <= 255) {
                if (gray_255 < 64)      val = 3;
                else if (gray_255 < 128) val = 2;
                else if (gray_255 < 192) val = 1;
                else                    val = 0;
            } else {
                // 透明/无效像素：跳过，解析下一个
                token = strtok(nullptr, " ");
                pngX++;
                continue;
            }

            // 6. 按当前渲染模式绘制：与drawBitmap判断逻辑完全一致
            // 清空区域
            if (renderMode == BW && val < 4) {
              drawPixel(screenX, screenY,false);
            }

          if (renderMode == BW && val < 3) {
            drawPixel(screenX, screenY);
          } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
            drawPixel(screenX, screenY, false);
          } else if (renderMode == GRAYSCALE_LSB && val == 1) {
            drawPixel(screenX, screenY, false);
          }

            // 解析下一个灰度值，x坐标+1
            token = strtok(nullptr, " ");
            pngX++;
        }

        // 读取下一行，y坐标+1
        pngY++;
    }

    // 7. 关闭文件，释放资源
    txtpng_file.close();
    Serial.printf("[%lu] [GFX] Png draw completed (mode: %d)\n", millis(), renderMode);
}

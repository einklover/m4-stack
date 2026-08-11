#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>
#include <EpdFontStyles.h>

#include <map>
#include <string>

#include "Bitmap.h"

#include "CustomEpdFont.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // logical coordinates (portrait)
    LandscapeClockwise,        // logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // logical coordinates, inverted
    LandscapeCounterClockwise  // logical coordinates, native panel orientation
  };

 private:
  // Chunk size derived from buffer size so it works for both X4 (48000/12=4000) and X3 (52272/12=4356)
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = HalDisplay::BUFFER_SIZE / 12;
  static constexpr size_t BW_BUFFER_NUM_CHUNKS = HalDisplay::BUFFER_SIZE / BW_BUFFER_CHUNK_SIZE;
  static_assert(BW_BUFFER_CHUNK_SIZE * BW_BUFFER_NUM_CHUNKS == HalDisplay::BUFFER_SIZE,
                "BW buffer chunking does not line up with display buffer size");

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  uint8_t* frameBuffer = nullptr;
  uint8_t* bwBufferChunks[BW_BUFFER_NUM_CHUNKS] = {nullptr};
  uint8_t* lastShownFrame = nullptr;  // persistent prev-page copy (PSRAM)
  mutable uint8_t* pendingEntryFrame = nullptr;  // one-shot gesture transition source (PSRAM)
  mutable int pendingEntryDirection = 0;  // logical direction; mapped at display time
  mutable uint32_t pendingEntryDeadlineMs = 0;
  uint8_t* rollingHalfBuffer = nullptr;  // for rolling auto-turn half-page blending
  std::map<int, EpdFontFamily> fontMap;
  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, const int* y, bool pixelState,
                  EpdFontFamily::Style style, float scale = 1.0f) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  // Insert-only: does not overwrite an existing fontId (fails silently if present).
  void insertFont(int fontId, EpdFontFamily font);
  // Explicit replace/upsert for M4 full-CJK epdfont promotion of NOTOSANS/UI IDs.
  void replaceFont(int fontId, EpdFontFamily font);
  bool hasFont(int fontId) const;
  // Borrow the currently mapped font object. Used by the M4 runtime-TTF loader
  // to capture the compact builtin UI faces before installing scaled views.
  // The renderer keeps ownership of the family mapping; callers must not free
  // the returned font.
  const EpdFont* getFontPtr(int fontId,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    const auto it = fontMap.find(fontId);
    return it == fontMap.end() ? nullptr : it->second.getFont(style);
  }
  // Remove a transient/custom mapping before a font reload. The owning font
  // object is managed by FontManager; erasing the value copy only drops the
  // renderer's pointer aliases.
  void removeFont(int fontId) { fontMap.erase(fontId); }
  // Exact coverage check used by plugin/UI text mapping.  Font backends may
  // return '?' for a missing glyph, so this distinguishes real coverage from
  // that fallback before choosing the reader face.
  bool hasTextGlyphs(int fontId, const char* text,
                     EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void clearCustomFonts(int startId = 1000);

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }
  // Convert a logical UI direction (R→L, L→R, B→T, T→B) to the physical
  // landscape panel direction consumed by M4WaveformLab.
  int logicalToPhysicalAnimationDirection(int logicalDirection) const;

  // Map normalized panel-native touch (0..1) into logical screen coordinates.
  // Panel frame is landscape 800x480; portrait UI is 480x800.
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  // Arm a one-shot animated transition for the next frame drawn by any activity.
  // Only the touch gesture path uses this; button navigation never arms it.
  void armEntryAnimation(int direction);
  void cancelEntryAnimation();
  void ageEntryAnimation();
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  // scale: 1.0 = native glyph pixels; <1 shrinks bitmaps (e.g. body epdfont → UI size).
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontStyles::REGULAR,
                   float scale = 1.0f) const;
  int getTextAdvance(int fontId, const char* text, EpdFontFamily::Style style = EpdFontStyles::REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR, float scale = 1.0f) const;
  int getSpaceWidth(int fontId) const;
  int getTextAdvanceX(int fontId, const char* text) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR, float scale = 1.0f) const;
  // Scale body/reader face metrics down to match a compact UI face (never upscales).
  float scaleFontToMatch(int srcFontId, int targetFontId) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;

  // Rolling half-page buffer: saves the logical top half of the framebuffer so
  // the caller can render another page and then blend the two halves together.
  bool preAllocateTopHalfBuffer();  // Pre-allocate buffer only (no content copy)
  bool saveTopHalfBuffer();      // Returns true if allocation succeeded
  void restoreTopHalfToFrame();  // Writes saved top half back into the framebuffer
  void freeTopHalfBuffer();      // Frees the rolling buffer
  
  // SD card version of half buffer (for memory-constrained scenarios)
  // Uses SD card instead of RAM to store the half buffer
  bool saveTopHalfToSd(const char* path);     // Save top half to SD card file
  bool restoreTopHalfFromSd(const char* path);  // Restore top half from SD card file

  // In-place masking helpers for grayscale compositing without extra allocation.
  // Both are orientation-aware: they zero out the logical top/bottom half of the
  // current frameBuffer so that copyGrayscale*Buffers() only writes gray data to
  // the intended half.  Pixels zeroed here map to LUT entry "00" (no waveform),
  // which means the display leaves those pixels at their BW-refresh state.
  void clearLogicalTopHalf();     // zeros the logical top half of frameBuffer
  void clearLogicalBottomHalf();  // zeros the logical bottom half of frameBuffer

  // Low level functions
  uint8_t* getFrameBuffer() const;
  // Access a stored previous-frame chunk (page-turn animation source).
  const uint8_t* bwBufferChunk(size_t i) const {
    return (i < BW_BUFFER_NUM_CHUNKS) ? bwBufferChunks[i] : nullptr;
  }
  // Persistent "last displayed frame" for page-turn animation.  Unlike the
  // AA scratch chunks (stored then freed by restoreBwBuffer), this copy
  // survives until the next display, so the animation can read the previous
  // page even after the new page has been rendered into frameBuffer.
  bool storeLastShown();
  const uint8_t* getLastShown() const { return lastShownFrame; }
  static size_t getBufferSize();
    //透明壁纸
  void drawPngFromTxtpng(const char* txtpng_file_path) const ;

};

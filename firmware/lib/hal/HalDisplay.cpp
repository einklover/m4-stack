#include <HalDisplay.h>
#include <HalGPIO.h>

#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
#include <esp_heap_caps.h>

namespace {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
constexpr uintptr_t kQemuEpdFrameAddress = 0x60100000;
constexpr uintptr_t kQemuEpdRefreshAddress = 0x6010C000;
#endif

void dumpQemuFrame(const uint8_t* buffer) {
  if (!buffer) return;
  static constexpr uint32_t kChunkBytes = 128;
  static constexpr char kHex[] = "0123456789ABCDEF";
  char line[kChunkBytes * 2 + 1];

  Serial.printf("[M4-QEMU-FB] BEGIN %u %u %lu\n", static_cast<unsigned>(HalDisplay::DISPLAY_WIDTH),
                static_cast<unsigned>(HalDisplay::DISPLAY_HEIGHT),
                static_cast<unsigned long>(HalDisplay::BUFFER_SIZE));
  for (uint32_t offset = 0; offset < HalDisplay::BUFFER_SIZE; offset += kChunkBytes) {
    const uint32_t remaining = HalDisplay::BUFFER_SIZE - offset;
    const uint32_t count = remaining < kChunkBytes ? remaining : kChunkBytes;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t value = buffer[offset + i];
      line[i * 2] = kHex[value >> 4];
      line[i * 2 + 1] = kHex[value & 0x0F];
    }
    line[count * 2] = '\0';
    Serial.printf("[M4-QEMU-FB] D %s\n", line);
  }
  Serial.println("[M4-QEMU-FB] END");
  Serial.flush();
}
}  // namespace
#endif

#ifdef CROSSPOINT_MURPHY_M4
// M4 pins come from BoardConfig::ACTIVE; ctor values are legacy and unused by FreeInkDisplay.
HalDisplay::HalDisplay() : einkDisplay(-1, -1, -1, -1, -1, -1) {}
#else
#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}
#endif

HalDisplay::~HalDisplay() {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  free(qemuFrameBuffer);
#endif
}

void HalDisplay::begin() {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  if (!qemuFrameBuffer) {
    qemuFrameBuffer = static_cast<uint8_t*>(heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM));
    if (!qemuFrameBuffer) qemuFrameBuffer = static_cast<uint8_t*>(malloc(BUFFER_SIZE));
  }
  if (qemuFrameBuffer) memset(qemuFrameBuffer, 0xFF, BUFFER_SIZE);
#else
#ifdef CROSSPOINT_X3
  einkDisplay.setDisplayX3();
#endif
#endif
  Serial.printf("[%lu] [M4-DISP] begin() panel=%ux%u buffer=%lu\n", millis(),
                static_cast<unsigned>(DISPLAY_WIDTH), static_cast<unsigned>(DISPLAY_HEIGHT),
                static_cast<unsigned long>(BUFFER_SIZE));
#ifndef HALDISPLAY_QEMU_FRAMEBUFFER
  einkDisplay.begin();
#endif
  if (getFrameBuffer() == nullptr) {
    Serial.printf("[%lu] [M4-DISP] ERROR: framebuffer allocation failed\n", millis());
  } else {
    Serial.printf("[%lu] [M4-DISP] framebuffer ready\n", millis());
  }
}

void HalDisplay::clearScreen(uint8_t color) const {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  if (qemuFrameBuffer) memset(qemuFrameBuffer, color, BUFFER_SIZE);
#else
  einkDisplay.clearScreen(color);
#endif
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  if (!qemuFrameBuffer) return;
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h && y + row < DISPLAY_HEIGHT; ++row) {
    const uint32_t dest = static_cast<uint32_t>(y + row) * DISPLAY_WIDTH_BYTES + x / 8;
    const uint32_t src = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes && x / 8 + col < DISPLAY_WIDTH_BYTES; ++col) {
      qemuFrameBuffer[dest + col] = fromProgmem ? pgm_read_byte(&imageData[src + col]) : imageData[src + col];
    }
  }
#else
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
#endif
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  if (!qemuFrameBuffer) return;
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h && y + row < DISPLAY_HEIGHT; ++row) {
    const uint32_t dest = static_cast<uint32_t>(y + row) * DISPLAY_WIDTH_BYTES + x / 8;
    const uint32_t src = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes && x / 8 + col < DISPLAY_WIDTH_BYTES; ++col) {
      const uint8_t value = fromProgmem ? pgm_read_byte(&imageData[src + col]) : imageData[src + col];
      qemuFrameBuffer[dest + col] &= value;
    }
  }
#else
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
#endif
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::UI_FAST_REFRESH:
      // Murphy SSD1677 has no separate UI-fast LUT; map to FAST (DU-class) which is
      // the proven low-latency path. Never silently no-op UI refreshes.
      return EInkDisplay::FAST_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  (void)mode;
  (void)turnOffScreen;
  if (!qemuFrameBuffer) return;
  memcpy(reinterpret_cast<void*>(kQemuEpdFrameAddress), qemuFrameBuffer, BUFFER_SIZE);
  *reinterpret_cast<volatile uint32_t*>(kQemuEpdRefreshAddress) = 1;
#elif defined(M4_QEMU_BUILD)
  (void)mode;
  (void)turnOffScreen;
  dumpQemuFrame(qemuFrameBuffer);
#else
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
#endif
}

bool HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  if (w == 0 || h == 0) return false;
  if ((x % 8) != 0 || (w % 8) != 0) return false;
  if (static_cast<uint32_t>(x) + w > DISPLAY_WIDTH || static_cast<uint32_t>(y) + h > DISPLAY_HEIGHT) {
    return false;
  }
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  (void)turnOffScreen;
  displayBuffer(FAST_REFRESH, false);
  return true;
#elif defined(M4_QEMU_BUILD)
  (void)turnOffScreen;
  displayBuffer(FAST_REFRESH, false);
  return true;
#else
  // Stock FreeInk API is void. Validation above is the production reject path.
  // SSD1677 displayWindow already Fast/DU-refreshes a byte-aligned window and,
  // in single-buffer mode, equalizes RED=BW in that window after activation.
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
  return true;
#endif
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  displayBuffer(mode, turnOffScreen);
#elif defined(M4_QEMU_BUILD)
  displayBuffer(mode, turnOffScreen);
#else
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
#endif
}

void HalDisplay::deepSleep() {
#ifndef HALDISPLAY_QEMU_FRAMEBUFFER
  einkDisplay.deepSleep();
#endif
}

uint8_t* HalDisplay::getFrameBuffer() const {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  return qemuFrameBuffer;
#else
  return einkDisplay.getFrameBuffer();
#endif
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  // The simulator surface is monochrome. Use the high bit as a stable 50%
  // threshold when a caller supplies the normal two grayscale planes.
  const uint8_t* plane = msbBuffer ? msbBuffer : lsbBuffer;
  if (qemuFrameBuffer && plane) memcpy(qemuFrameBuffer, plane, BUFFER_SIZE);
#else
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
#endif
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  if (qemuFrameBuffer && lsbBuffer) memcpy(qemuFrameBuffer, lsbBuffer, BUFFER_SIZE);
#else
  einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer);
#endif
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  if (qemuFrameBuffer && msbBuffer) memcpy(qemuFrameBuffer, msbBuffer, BUFFER_SIZE);
#else
  einkDisplay.copyGrayscaleMsbBuffers(msbBuffer);
#endif
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  if (qemuFrameBuffer && bwBuffer) memcpy(qemuFrameBuffer, bwBuffer, BUFFER_SIZE);
#else
  einkDisplay.cleanupGrayscaleBuffers(bwBuffer);
#endif
}

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  displayBuffer(HALF_REFRESH, turnOffScreen);
#else
  einkDisplay.displayGrayBuffer(turnOffScreen);
#endif
}

uint32_t HalDisplay::waveformLabRefresh(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                                        bool turnOff) {
  return einkDisplay.waveformLabRefresh(prev, next, lut, turnOff);
}

void HalDisplay::waveformLabBaseline(const uint8_t* frame) {
  einkDisplay.waveformLabBaseline(frame);
}

uint32_t HalDisplay::waveformLabRefreshWindow(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                                              uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                              bool syncAfter) {
  return einkDisplay.waveformLabRefreshWindow(prev, next, lut, x, y, w, h, syncAfter);
}

uint32_t HalDisplay::waveformLabRefreshWindowBufs(const uint8_t* redWin, const uint8_t* bwWin,
                                                  const uint8_t* lut, uint16_t x, uint16_t y, uint16_t w,
                                                  uint16_t h) {
  return einkDisplay.waveformLabRefreshWindowBufs(redWin, bwWin, lut, x, y, w, h);
}

void HalDisplay::waveformLabWriteDiffWindow(const uint8_t* prev, const uint8_t* next, uint16_t x, uint16_t y,
                                            uint16_t w, uint16_t h) {
  einkDisplay.waveformLabWriteDiffWindow(prev, next, x, y, w, h);
}

uint32_t HalDisplay::waveformLabActivate(const uint8_t* lut) {
  return einkDisplay.waveformLabActivate(lut);
}

uint32_t HalDisplay::waveformLabActivateWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                               const uint8_t* lut) {
  return einkDisplay.waveformLabActivateWindow(x, y, w, h, lut);
}

void HalDisplay::waveformLabEqualizeWindow(const uint8_t* frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.waveformLabEqualizeWindow(frame, x, y, w, h);
}

void HalDisplay::setCustomLUT(bool enabled, const unsigned char* lutData) {
  einkDisplay.setCustomLUT(enabled, lutData);
}

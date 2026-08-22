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

HalDisplay::RefreshMode normalizeRefreshMode(HalDisplay::RefreshMode mode,
                                              HalDisplay::RefreshContext context) {
  if (mode == HalDisplay::READER_CLEANUP_REFRESH &&
      context == HalDisplay::READER_BODY_CONTEXT) {
    return mode;
  }
  // FULL/HALF are retained as source-compatible enum names only. They must not
  // reach the panel driver, because older drivers attach multi-phase waveforms
  // to those names. UI/plugin/loading paths therefore always become FAST.
  return HalDisplay::FAST_REFRESH;
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  if (mode == HalDisplay::READER_CLEANUP_REFRESH) {
    // The generated SDK is intentionally not committed. Its policy-aware
    // enum appends cleanup at slot 3; older SDKs reject no symbol here and
    // route the unknown slot through their FAST/default case.
    constexpr int kReaderCleanupRefreshSlot = 3;
    return static_cast<EInkDisplay::RefreshMode>(kReaderCleanupRefreshSlot);
  }
  return EInkDisplay::FAST_REFRESH;
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen,
                               HalDisplay::RefreshContext context) {
  const HalDisplay::RefreshMode effectiveMode = normalizeRefreshMode(mode, context);
  Serial.printf("[%lu] [M4-DISP] display requested=%d effective=%d context=%d\n",
                millis(), static_cast<int>(mode), static_cast<int>(effectiveMode),
                static_cast<int>(context));
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  (void)turnOffScreen;
  if (!qemuFrameBuffer) return;
  memcpy(reinterpret_cast<void*>(kQemuEpdFrameAddress), qemuFrameBuffer, BUFFER_SIZE);
  *reinterpret_cast<volatile uint32_t*>(kQemuEpdRefreshAddress) = 1;
#elif defined(M4_QEMU_BUILD)
  (void)turnOffScreen;
  dumpQemuFrame(qemuFrameBuffer);
#else
  einkDisplay.displayBuffer(convertRefreshMode(effectiveMode), turnOffScreen);
#endif
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen,
                                HalDisplay::RefreshContext context) {
  const HalDisplay::RefreshMode effectiveMode = normalizeRefreshMode(mode, context);
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  displayBuffer(effectiveMode, turnOffScreen, context);
#elif defined(M4_QEMU_BUILD)
  displayBuffer(effectiveMode, turnOffScreen, context);
#else
  Serial.printf("[%lu] [M4-DISP] refresh requested=%d effective=%d context=%d\n",
                millis(), static_cast<int>(mode), static_cast<int>(effectiveMode),
                static_cast<int>(context));
  einkDisplay.refreshDisplay(convertRefreshMode(effectiveMode), turnOffScreen);
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
  displayBuffer(FAST_REFRESH, turnOffScreen, UI_CONTEXT);
#else
  einkDisplay.displayGrayBuffer(turnOffScreen);
#endif
}

uint32_t HalDisplay::waveformLabRefresh(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                                        bool turnOff) {
  (void)lut;
  // The SDK facade can otherwise arm a caller-supplied LUT internally. LUTs
  // are diagnostic data only; the global policy permits the fast waveform.
  return einkDisplay.waveformLabRefresh(prev, next, nullptr, turnOff);
}

void HalDisplay::waveformLabBaseline(const uint8_t* frame) {
  if (!frame) return;
  // The SDK's legacy baseline helper selects FULL. A same-frame FAST pass
  // establishes the lab baseline without a multi-phase absolute waveform.
  (void)einkDisplay.waveformLabRefresh(frame, frame, nullptr, false);
}

uint32_t HalDisplay::waveformLabRefreshWindow(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                                              uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                              bool syncAfter) {
  (void)lut;
  return einkDisplay.waveformLabRefreshWindow(prev, next, nullptr, x, y, w, h, syncAfter);
}

uint32_t HalDisplay::waveformLabRefreshWindowBufs(const uint8_t* redWin, const uint8_t* bwWin,
                                                  const uint8_t* lut, uint16_t x, uint16_t y, uint16_t w,
                                                  uint16_t h) {
  (void)lut;
  return einkDisplay.waveformLabRefreshWindowBufs(redWin, bwWin, nullptr, x, y, w, h);
}

void HalDisplay::waveformLabWriteDiffWindow(const uint8_t* prev, const uint8_t* next, uint16_t x, uint16_t y,
                                            uint16_t w, uint16_t h) {
  einkDisplay.waveformLabWriteDiffWindow(prev, next, x, y, w, h);
}

uint32_t HalDisplay::waveformLabActivate(const uint8_t* lut) {
  (void)lut;
  return einkDisplay.waveformLabActivate(nullptr);
}

uint32_t HalDisplay::waveformLabActivateWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                               const uint8_t* lut) {
  (void)lut;
  return einkDisplay.waveformLabActivateWindow(x, y, w, h, nullptr);
}

void HalDisplay::waveformLabEqualizeWindow(const uint8_t* frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.waveformLabEqualizeWindow(frame, x, y, w, h);
}

void HalDisplay::setCustomLUT(bool enabled, const unsigned char* lutData) {
  (void)enabled;
  (void)lutData;
  // Prevent the legacy SDK escape hatch even for old debug-bridge commands.
  einkDisplay.setCustomLUT(false, nullptr);
}

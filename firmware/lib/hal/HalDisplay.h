#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

#if defined(M4_QEMU_BUILD) || (defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG)
#define HALDISPLAY_QEMU_FRAMEBUFFER 1
#endif

class HalDisplay {
 public:
  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,     // Full refresh with complete waveform
    HALF_REFRESH,     // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH,     // Fast refresh using custom LUT (best for reading)
    UI_FAST_REFRESH   // UI-optimized fast refresh (reduced flicker for menus)
  };

  // Initialize the display hardware and driver
  void begin();

  // Display dimensions (selected at compile time based on target device)
#ifdef CROSSPOINT_X3
  static constexpr uint16_t DISPLAY_WIDTH  = EInkDisplay::X3_DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::X3_DISPLAY_HEIGHT;
#else
  // X4 and Murphy M4 share the 800x480 panel geometry (logical portrait 480x800).
  static constexpr uint16_t DISPLAY_WIDTH  = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
#endif
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);

  // Waveform Lab: run one prev->next transition with a caller-supplied
  // 110-byte LUT.  Returns BUSY wait elapsed ms (0 if unsupported).
  uint32_t waveformLabRefresh(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                              bool turnOff = false);
  // Waveform Lab: absolute FULL refresh to the given frame (baseline setup).
  void waveformLabBaseline(const uint8_t* frame);
  // Waveform Lab: windowed diff refresh (strip-by-strip page-turn animation).
  uint32_t waveformLabRefreshWindow(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                                    uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                    bool syncAfter = true);
  uint32_t waveformLabRefreshWindowBufs(const uint8_t* redWin, const uint8_t* bwWin, const uint8_t* lut,
                                        uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  // Dual-dose strip pipeline: writeDiff → activate → equalize (selective).
  void waveformLabWriteDiffWindow(const uint8_t* prev, const uint8_t* next, uint16_t x, uint16_t y, uint16_t w,
                                  uint16_t h);
  uint32_t waveformLabActivate(const uint8_t* lut = nullptr);
  // Window-scoped activate: limits the SSD1677 master activation scan to the
  // given rectangle (setRamArea), restores the full-panel window afterwards.
  // Used for local page-turn wipes so status/other regions are never re-driven.
  uint32_t waveformLabActivateWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                     const uint8_t* lut = nullptr);
  void waveformLabEqualizeWindow(const uint8_t* frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void setCustomLUT(bool enabled, const unsigned char* lutData = nullptr);

 private:
#ifdef HALDISPLAY_QEMU_FRAMEBUFFER
  uint8_t* qemuFrameBuffer = nullptr;
#endif
  EInkDisplay einkDisplay;
};

#include <HalGPIO.h>
#include <SPI.h>
#include <esp_sleep.h>

#ifdef CROSSPOINT_MURPHY_M4
#include <BoardConfig.h>
#include <PowerManager.h>
#endif

void HalGPIO::begin() {
#ifdef CROSSPOINT_MURPHY_M4
  // FreeInk InputManager / BoardConfig own M4 pin mux. Do not SPI.begin() X4 pins
  // (display/SD share is an X4 assumption; M4 uses dedicated EPD SPI + SDMMC).
  Serial.printf("[%lu] [M4-GPIO] begin() FREEINK_DEVICE_MURPHY_M4 board=%s\n", millis(),
                BoardConfig::ACTIVE.name);
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  inputMgr.begin();
  Serial.printf("[%lu] [M4-TOUCH] configured=%d stream_ready=%d (settle may still be pending)\n",
                millis(), hasTouch() ? 1 : 0, isTouchStreamReady() ? 1 : 0);
#else
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  pinMode(UART0_RXD, INPUT);
#endif
}

bool HalGPIO::isTouchStreamReady() const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.isTouchStreamReady();
#else
  return false;
#endif
}

void HalGPIO::update() {
  // Save previous virtual button state BEFORE updating current state
  // This allows wasReleased() to detect buttons that were pressed last frame but not this frame
  previousVirtualButtonEvents = virtualButtonEvents;

  // Move queued virtual buttons to current events for this frame
  // Then clear the queue so only buttons pressed this frame show in events
  virtualButtonEvents = virtualButtonQueue;
  virtualButtonQueue = 0;

  inputMgr.update();
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  return inputMgr.wasPressed(buttonIndex) || (virtualButtonEvents & (1 << buttonIndex));
}

bool HalGPIO::wasAnyPressed() const {
  return inputMgr.wasAnyPressed() || (virtualButtonEvents > 0);
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  // Check both physical button releases AND virtual button releases
  // Virtual release = was pressed last frame but not this frame
  const uint8_t virtualRelease = previousVirtualButtonEvents & ~virtualButtonEvents;
  return inputMgr.wasReleased(buttonIndex) || (virtualRelease & (1 << buttonIndex));
}

bool HalGPIO::wasAnyReleased() const {
  // Check both physical and virtual button releases
  const uint8_t virtualRelease = previousVirtualButtonEvents & ~virtualButtonEvents;
  return inputMgr.wasAnyReleased() || (virtualRelease > 0);
}

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

bool HalGPIO::hasTouch() const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.hasTouch();
#else
  return false;
#endif
}

bool HalGPIO::wasTouchTap(float& nx, float& ny) const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.wasTouchTap(nx, ny);
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool HalGPIO::wasTouchDown(float& nx, float& ny) const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.wasTouchPressedAt(nx, ny);
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
#else
  (void)nx;
  (void)ny;
  (void)heldMs;
  return false;
#endif
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.isTouchHeldAt(nx, ny);
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

unsigned long HalGPIO::lastTouchHeldMs() const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.lastTouchHeldMs();
#else
  return 0;
#endif
}

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
#else
  (void)nxStart;
  (void)nyStart;
  (void)nxEnd;
  (void)nyEnd;
  return false;
#endif
}

bool HalGPIO::wasTouchActivity() const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.wasTouchActivity();
#else
  return false;
#endif
}

bool HalGPIO::isTouchPressed() const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.isTouchPressed();
#else
  return false;
#endif
}

bool HalGPIO::wasTouchPressed() const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.wasTouchPressed();
#else
  return false;
#endif
}

bool HalGPIO::wasTouchReleased() const {
#ifdef CROSSPOINT_MURPHY_M4
  return inputMgr.wasTouchReleased();
#else
  return false;
#endif
}

bool HalGPIO::getTouchPanelPoint(int& x, int& y) const {
#ifdef CROSSPOINT_MURPHY_M4
  const InputManager::TouchPoint pt = inputMgr.getTouchPoint();
  if (!pt.valid) return false;
  x = static_cast<int>(pt.x);
  y = static_cast<int>(pt.y);
  return true;
#else
  (void)x;
  (void)y;
  return false;
#endif
}

void HalGPIO::startDeepSleep() {
#ifdef CROSSPOINT_MURPHY_M4
  // Use verified FreeInk power manager: correct S3 EXT1 wake, power-rail hold,
  // wait-for-release. Do not reuse X3/X4 GPIO wakeup constants.
  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
#else
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
#endif
}

int HalGPIO::getBatteryPercentage() const {
#ifdef CROSSPOINT_MURPHY_M4
  static const BatteryMonitor battery;
  return battery.readPercentage();
#else
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);
  return battery.readPercentage();
#endif
}


void HalGPIO::injectButtonPress(uint8_t buttonIndex) {
  // Queue the button for the next update() call
  // This ensures the reader gets a chance to check wasPressed()
  // before the button is cleared
  virtualButtonQueue |= (1 << buttonIndex);
}

void HalGPIO::clearVirtualButtons() {
  virtualButtonEvents = 0;
  virtualButtonQueue = 0;
}

bool HalGPIO::isUsbConnected() const {
#ifdef CROSSPOINT_MURPHY_M4
  // M4 has no independently verified USB-present GPIO in BoardConfig.
  if (BoardConfig::ACTIVE.usbDetect < 0) {
    return false;
  }
  return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
#else
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
#endif
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

#ifdef CROSSPOINT_MURPHY_M4
  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  // Fresh flash / cold boot on M4: no safe USB detect — do not force power-button
  // duration verification that would re-sleep immediately.
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON) {
    return WakeupReason::Other;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN) {
    return WakeupReason::AfterFlash;
  }
  return WakeupReason::Other;
#else
  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
#endif
}

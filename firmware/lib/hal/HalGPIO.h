#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
// On Murphy M4 these macros are not used for bus setup; FreeInk BoardConfig owns pins.
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage (X4 ADC)

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware - I2C bus (shared by fuel gauge BQ27220 and IMU QMI8658)
#define X3_I2C_SDA  20
#define X3_I2C_SCL  0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C registers (X3 battery monitoring)
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG  0x2C  // StateOfCharge() command code (%)
#define BQ27220_CUR_REG  0x0C  // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif
  uint8_t virtualButtonEvents = 0;   // Current frame virtual button events
  uint8_t virtualButtonQueue = 0;    // Persistent queue for virtual buttons
  uint8_t previousVirtualButtonEvents = 0;  // Track previous frame for release detection

 public:
  HalGPIO() = default;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;

  // Capacitive touch (inert / false on X3/X4; live on Murphy M4)
  bool hasTouch() const;           // controller configured
  bool isTouchStreamReady() const; // settle + successful probe/frame
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool isTouchHeldAt(float& nx, float& ny) const;
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // Get battery percentage (range 0-100)
  int getBatteryPercentage() const;

  // Virtual button injection (for Bluetooth HID)
  void injectButtonPress(uint8_t buttonIndex);
  void clearVirtualButtons();

  // Check if USB is connected
  bool isUsbConnected() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

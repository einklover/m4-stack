#pragma once
#include <Arduino.h>
#include <functional>

class TiltPageTurnDetector {
 public:
  enum class Action { NONE, PREV_PAGE, NEXT_PAGE, TOGGLE_AUTO_PAGE_TURN, OPEN_MENU, TAP_ACTION, AUTO_ROTATE };
  using ActionCallback = std::function<void(Action)>;

  TiltPageTurnDetector() = default;

  /// Initialize I2C and QMI8658 sensor. Returns true if sensor found.
  bool begin(int sdaPin, int sclPin);

  /// Call from main loop(). Reads sensor and runs tilt state machine.
  void update();

  /// Set callback for triggered actions (page turn, menu, etc.)
  void setActionCallback(ActionCallback cb) { actionCallback = std::move(cb); }

  /// Suspend/resume detection (e.g. when not in reader and scope=仅阅读)
  void setSuspended(bool s) { suspended = s; }

  bool isReady() const { return imuReady; }

  /// Get the currently detected physical orientation (CrossPointSettings::ORIENTATION value)
  uint8_t getDetectedOrientation() const { return detectedOrientation; }

 private:
  // QMI8658 registers
  static constexpr uint8_t REG_WHO_AM_I = 0x00;
  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL2 = 0x03;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_RESET = 0x60;
  static constexpr uint8_t REG_AX_L = 0x35;

  enum class PageState { IDLE, TILTING, TRIGGERED, COOLDOWN };
  enum class TiltDir { NONE, LEFT, RIGHT };

  // Wire I2C helpers (shared bus with HalPowerManager)
  bool i2cInit();
  void i2cDeinit();
  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t* out);
  bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len);

  bool initIMU();
  bool readAccelData(float& ax, float& ay, float& az);
  float calcRoll(float ax, float ay, float az);

  void processPageTurn(float roll, unsigned long now);
  void processTap(float ax, float ay, float az, unsigned long now);
  void processAutoRotate(float ax, float ay, float az, unsigned long now);
  Action resolveAction(TiltDir dir, bool isLarge);

  uint8_t imuAddr = 0;
  float accelScale = 0.0f;
  bool imuReady = false;
  bool driverInstalled = false;
  bool suspended = false;
  int i2cSda = 20;
  int i2cScl = 0;

  unsigned long lastSampleTime = 0;
  unsigned long lastRetryTime = 0;
  uint32_t consecutiveFailures = 0;

  PageState pageState = PageState::IDLE;
  TiltDir tiltDir = TiltDir::NONE;
  bool largeTilt = false;
  unsigned long tiltStartTime = 0;
  unsigned long cooldownStartTime = 0;

  ActionCallback actionCallback;

  // Tap detection state
  float baselineMag = 0.0f;          // EMA基线加速度幅值(g)
  bool tapArmed = true;              // 是否允许检测新的tap
  unsigned long lastTapTime = 0;     // 上次tap触发时间
  uint8_t tiltSampleCounter = 0;     // tilt降频计数器

  // Auto-rotate state
  uint8_t detectedOrientation = 0;   // 当前检测到的方向 (CrossPointSettings::ORIENTATION)
  uint8_t pendingOrientation = 0xFF; // 待确认的新方向 (0xFF=无)
  unsigned long orientationStableTime = 0;  // 新方向开始稳定的时间
  static constexpr uint32_t ORIENTATION_STABLE_MS = 0;  // 检测到方向变化立即切换

  static constexpr uint32_t TAP_SAMPLE_INTERVAL_MS = 10;   // tap采样间隔(10ms=100Hz)
  static constexpr uint8_t TILT_SAMPLE_DIVIDER = 5;        // tilt每5次tap采样执行一次(50ms)
  static constexpr uint32_t SAMPLE_INTERVAL_MS = 50;
  static constexpr uint32_t RETRY_INTERVAL_MS = 3000;
  static constexpr float LARGE_TILT_ANGLE = 70.0f;
  static constexpr uint32_t MAX_CONSECUTIVE_FAILURES = 10;
  static constexpr uint32_t I2C_TIMEOUT_MS = 50;
};

#include "TiltPageTurnDetector.h"

#include <cmath>
#include <Wire.h>

#include "CrossPointSettings.h"

// ==================== Wire I2C (shared with HalPowerManager) ====================

bool TiltPageTurnDetector::i2cInit() {
  // Wire is already initialized by HalPowerManager::begin(), nothing to do here.
  // Just mark as ready so the rest of the code knows I2C is available.
  driverInstalled = true;
  return true;
}

void TiltPageTurnDetector::i2cDeinit() {
  // No-op: Wire bus is shared with battery fuel gauge, do not tear it down.
  // driverInstalled stays true since Wire persists.
}

bool TiltPageTurnDetector::writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool TiltPageTurnDetector::readReg(uint8_t reg, uint8_t* out) {
  return readRegs(reg, out, 1);
}

bool TiltPageTurnDetector::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  Wire.requestFrom(imuAddr, len);
  if (Wire.available() < len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

// ==================== Sensor init ====================

bool TiltPageTurnDetector::initIMU() {
  // Probe address 0x6A, then 0x6B using Wire
  for (uint8_t addr : {0x6A, 0x6B}) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      imuAddr = addr;
      Serial.printf("[TILT] QMI8658 addr: 0x%02X\n", imuAddr);
      break;
    }
  }
  if (imuAddr == 0) {
    Serial.println("[TILT] QMI8658 not found at 0x6A/0x6B");
    return false;
  }

  uint8_t whoami = 0;
  if (!readReg(REG_WHO_AM_I, &whoami) || whoami != 0x05) {
    Serial.printf("[TILT] WHO_AM_I: 0x%02X (expected 0x05)\n", whoami);
    imuAddr = 0;
    return false;
  }
  Serial.printf("[TILT] WHO_AM_I: 0x%02X\n", whoami);

  // Soft reset
  writeReg(REG_RESET, 0xB0);
  delay(50);

  // CTRL1=0x40: little-endian + address auto-increment
  writeReg(REG_CTRL1, 0x40);

  // CTRL2=0x34: accelerometer ±8g, 250Hz
  writeReg(REG_CTRL2, 0x34);
  accelScale = 8.0f * 9.80665f / 32768.0f;

  // CTRL7=0x01: accelerometer only (gyro off)
  writeReg(REG_CTRL7, 0x01);

  Serial.println("[TILT] QMI8658 ready (accel only, gyro off)");
  return true;
}

// ==================== Data reading ====================

bool TiltPageTurnDetector::readAccelData(float& ax, float& ay, float& az) {
  uint8_t buf[6];
  if (!readRegs(REG_AX_L, buf, 6)) return false;

  int16_t rawAx = (int16_t)(buf[0] | (buf[1] << 8));
  int16_t rawAy = (int16_t)(buf[2] | (buf[3] << 8));
  int16_t rawAz = (int16_t)(buf[4] | (buf[5] << 8));

  ax = rawAx * accelScale;
  ay = rawAy * accelScale;
  az = rawAz * accelScale;
  return true;
}

float TiltPageTurnDetector::calcRoll(float ax, float ay, float az) {
  float ay_g = ay / 9.80665f;
  float ax_g = ax / 9.80665f;
  float az_g = az / 9.80665f;
  return atan2(ay_g, sqrt(ax_g * ax_g + az_g * az_g)) * 57.2958f;
}

// ==================== Public interface ====================

bool TiltPageTurnDetector::begin(int sdaPin, int sclPin) {
  i2cSda = sdaPin;
  i2cScl = sclPin;

  if (!i2cInit()) return false;

  if (initIMU()) {
    imuReady = true;
    delay(100);
    // Discard first reading
    float d1, d2, d3;
    readAccelData(d1, d2, d3);
    lastSampleTime = millis();
    Serial.println("[TILT] Tilt page turn detector initialized");
    return true;
  }

  // Sensor not found, release I2C so pins can be used for other purposes
  i2cDeinit();
  Serial.println("[TILT] QMI8658 not ready, will retry periodically");
  return false;
}

void TiltPageTurnDetector::update() {
  // 只要倾斜翻页或敲击翻页任一启用，就需要读取传感器
  bool needSensor = SETTINGS.tiltPageTurnEnabled || SETTINGS.tapPageTurnEnabled || SETTINGS.autoRotateEnabled;
  
  if (!needSensor || suspended) {
    // If was running, release I2C resources
    if (imuReady) {
      i2cDeinit();
      imuReady = false;
      pageState = PageState::IDLE;
      Serial.println("[TILT] Tilt/Tap stopped, released I2C");
    }
    return;
  }

  unsigned long now = millis();

  // Lazy init: sensor not yet initialized (first enable or after disable)
  if (!imuReady) {
    if (now - lastRetryTime >= RETRY_INTERVAL_MS) {
      lastRetryTime = now;
      if (i2cInit() && initIMU()) {
        imuReady = true;
        consecutiveFailures = 0;
        delay(100);
        float d1, d2, d3;
        readAccelData(d1, d2, d3);
        lastSampleTime = millis();
      } else {
        i2cDeinit();
      }
    }
    return;
  }

  // Sample at fixed interval (10ms = 100Hz, 足够捕获手指敲击的短脉冲)
  if (now - lastSampleTime < TAP_SAMPLE_INTERVAL_MS) return;
  lastSampleTime = now;

  // Wire bus is shared with HalPowerManager (fuel gauge), no init/deinit needed per read.
  float ax, ay, az;
  if (!readAccelData(ax, ay, az)) {
    consecutiveFailures++;
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
      Serial.printf("[TILT] %d consecutive read failures, marking IMU not ready\n", consecutiveFailures);
      imuReady = false;
      consecutiveFailures = 0;
      lastRetryTime = millis();
    }
    return;
  }
  consecutiveFailures = 0;

  // Tap 检测每次采样都执行（10ms）
  processTap(ax, ay, az, now);
  
  // Tilt 检测降频到每 50ms 执行一次（不需要那么高的采样率）
  tiltSampleCounter++;
  if (tiltSampleCounter >= TILT_SAMPLE_DIVIDER) {
    tiltSampleCounter = 0;
    if (SETTINGS.tiltPageTurnEnabled) {
      float roll = calcRoll(ax, ay, az);
      processPageTurn(roll, now);
    }
    // 自动旋转也用 50ms 频率检测（不需要太快）
    processAutoRotate(ax, ay, az, now);
  }
}

// ==================== State machine ====================

void TiltPageTurnDetector::processPageTurn(float roll, unsigned long now) {
  const float triggerAngle = (float)SETTINGS.tiltTriggerAngle;
  const float releaseAngle = (float)SETTINGS.tiltReleaseAngle;
  const uint32_t holdTime = SETTINGS.tiltHoldTimeMs;
  const uint32_t cooldown = SETTINGS.tiltCooldownTimeMs;

  // Determine current tilt direction
  TiltDir currentDir = TiltDir::NONE;
  if (roll > triggerAngle)
    currentDir = TiltDir::RIGHT;
  else if (roll < -triggerAngle)
    currentDir = TiltDir::LEFT;

  bool isFlat = (fabs(roll) < releaseAngle);
  bool isLarge = (fabs(roll) > LARGE_TILT_ANGLE);

  switch (pageState) {
    case PageState::IDLE:
      if (currentDir != TiltDir::NONE) {
        pageState = PageState::TILTING;
        tiltDir = currentDir;
        largeTilt = isLarge;
        tiltStartTime = now;
      }
      break;

    case PageState::TILTING:
      if (currentDir != tiltDir) {
        // Direction changed or returned to flat -> reset
        pageState = PageState::IDLE;
        tiltDir = TiltDir::NONE;
      } else {
        // Track if tilt becomes large during hold period
        if (isLarge) largeTilt = true;
        if (now - tiltStartTime >= holdTime) {
          pageState = PageState::TRIGGERED;
        }
      }
      break;

    case PageState::TRIGGERED: {
      Action action = resolveAction(tiltDir, largeTilt);
      if (action != Action::NONE && actionCallback) {
        actionCallback(action);
      }
      // Enter cooldown
      pageState = PageState::COOLDOWN;
      cooldownStartTime = now;
      tiltDir = TiltDir::NONE;
      largeTilt = false;
      break;
    }

    case PageState::COOLDOWN:
      if (now - cooldownStartTime >= cooldown && isFlat) {
        pageState = PageState::IDLE;
      }
      break;
  }
}

TiltPageTurnDetector::Action TiltPageTurnDetector::resolveAction(TiltDir dir, bool isLarge) {
  if (dir == TiltDir::NONE) return Action::NONE;

  if (isLarge) {
    // Large tilt (>70°) - use large action settings
    uint8_t actionCode = (dir == TiltDir::LEFT) ? SETTINGS.tiltLargeLeftAction : SETTINGS.tiltLargeRightAction;
    switch (actionCode) {
      case 0: return Action::PREV_PAGE;
      case 1: return Action::NEXT_PAGE;
      case 2: return Action::TOGGLE_AUTO_PAGE_TURN;
      case 3: return Action::OPEN_MENU;
      default: return Action::NONE;
    }
  } else {
    // Normal tilt - use left/right action settings
    uint8_t actionCode = (dir == TiltDir::LEFT) ? SETTINGS.tiltLeftAction : SETTINGS.tiltRightAction;
    switch (actionCode) {
      case 0: return Action::PREV_PAGE;
      case 1: return Action::NEXT_PAGE;
      default: return Action::NONE;
    }
  }
}

// ==================== Tap detection ====================

void TiltPageTurnDetector::processTap(float ax, float ay, float az, unsigned long now) {
  if (!SETTINGS.tapPageTurnEnabled) return;
  
  // 倾斜状态机正在工作时，抑制tap检测
  if (pageState == PageState::TILTING || pageState == PageState::TRIGGERED) {
    tapArmed = false;
    return;
  }
  
  // 计算加速度矢量总幅值(g)
  const float G = 9.80665f;
  float mag = sqrtf(ax * ax + ay * ay + az * az) / G;
  
  // 用 EMA（指数移动平均）跟踪基线，alpha=0.05 → 慢速跟踪重力方向变化
  // 初始化时直接用当前值
  if (baselineMag < 0.1f) {
    baselineMag = mag;
  } else {
    baselineMag = 0.95f * baselineMag + 0.05f * mag;
  }
  
  // 偏离基线的绝对值 = 冲击信号
  float impact = fabsf(mag - baselineMag);
  
  // 阈值：设置值是0.01g单位，转换为g
  float threshold = (float)SETTINGS.tapThresholdG / 100.0f;
  
  // 冷却期间不检测，但继续更新基线
  if (now - lastTapTime < SETTINGS.tapCooldownMs) {
    tapArmed = false;
    return;
  }
  
  // 冷却结束后，等待加速度回到平静状态才重新arm
  if (!tapArmed) {
    if (impact < 0.08f) {  // 冲击小于0.08g认为平静
      tapArmed = true;
    }
    return;
  }
  
  // 检测冲击：偏离基线超过阈值
  if (impact > threshold) {
    Serial.printf("[TAP] Tap detected! impact=%.2fg baseline=%.2fg mag=%.2fg (threshold=%.1fg)\n", 
                  impact, baselineMag, mag, threshold);
    lastTapTime = now;
    tapArmed = false;
    
    Action action = (SETTINGS.tapAction == 0) ? Action::PREV_PAGE : Action::NEXT_PAGE;
    if (actionCallback) {
      actionCallback(action);
    }
  }
}

// ==================== Auto-rotate detection ====================

void TiltPageTurnDetector::processAutoRotate(float ax, float ay, float az, unsigned long now) {
  if (!SETTINGS.autoRotateEnabled) return;
  
  const float G = 9.80665f;
  float ax_g = ax / G;
  float ay_g = ay / G;
  float az_g = az / G;
  
  // 根据重力方向判断设备物理朝向
  // QMI8658 在 X3 设备上的轴向（根据 calcRoll 使用 ay 做左右倾斜推断）：
  //   ax: 上下方向（竖屏正常持握时 ax ≈ ±1g）
  //   ay: 左右方向（横屏时 ay ≈ ±1g）
  //   az: 屏幕法线方向
  
  // 确定当前物理方向
  // 阈值 35° 避免在临界角度反复切换
  uint8_t physicalOrientation;
  float axAngle = fabsf(atan2f(ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * 57.2958f);
  float ayAngle = fabsf(atan2f(ay_g, sqrtf(ax_g * ax_g + az_g * az_g)) * 57.2958f);
  
  if (axAngle > 35.0f && axAngle > ayAngle) {
    // ax 方向承受主要重力 → 竖屏（正负翻转：ax>0=倒置，ax<0=正常）
    physicalOrientation = (ax_g < 0) 
      ? CrossPointSettings::ORIENTATION::PORTRAIT 
      : CrossPointSettings::ORIENTATION::INVERTED;
  } else if (ayAngle > 35.0f && ayAngle > axAngle) {
    // ay 方向承受主要重力 → 横屏
    physicalOrientation = (ay_g > 0) 
      ? CrossPointSettings::ORIENTATION::LANDSCAPE_CW 
      : CrossPointSettings::ORIENTATION::LANDSCAPE_CCW;
  } else {
    // 设备接近水平放置，不改变方向
    pendingOrientation = 0xFF;
    return;
  }
  
  // 如果检测到的方向和当前设置一致，清除 pending
  if (physicalOrientation == SETTINGS.orientation) {
    pendingOrientation = 0xFF;
    detectedOrientation = physicalOrientation;
    return;
  }
  
  // 检测到新方向
  if (physicalOrientation != pendingOrientation) {
    // 方向刚变化，开始计时
    pendingOrientation = physicalOrientation;
    orientationStableTime = now;
  } else if (now - orientationStableTime >= ORIENTATION_STABLE_MS) {
    // 新方向已稳定足够长时间，触发切换
    detectedOrientation = physicalOrientation;
    pendingOrientation = 0xFF;
    
    Serial.printf("[ROTATE] Auto-rotate: %d -> %d\n", SETTINGS.orientation, detectedOrientation);
    
    if (actionCallback) {
      actionCallback(Action::AUTO_ROTATE);
    }
  }
}

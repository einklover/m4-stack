#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <ctime>

/**
 * DS3231 RTC 驱动（I2C 地址 0x68）
 *
 * 提供读取/设置时间功能，用于 X3 设备开机恢复系统时钟
 * 和 NTP 同步后回写 RTC。
 */
class DS3231RTC {
public:
    static constexpr uint8_t I2C_ADDR = 0x68;

    /// 检测 RTC 是否存在于 I2C 总线上
    static bool detect();

    /// 从 RTC 读取当前时间，返回 Unix 时间戳（UTC）
    /// 失败返回 0
    static time_t readTime();

    /// 将 Unix 时间戳（UTC）写入 RTC
    static bool writeTime(time_t unixTime);

    /// 从 RTC 读取时间并设置 ESP32 系统时钟
    /// 返回是否成功
    static bool syncSystemFromRTC();

    /// 将当前系统时钟写入 RTC（NTP 同步后调用）
    /// 返回是否成功
    static bool syncRTCFromSystem();

private:
    static uint8_t bcd2dec(uint8_t bcd);
    static uint8_t dec2bcd(uint8_t dec);
    static uint8_t readRegister(uint8_t reg);
    static void writeRegister(uint8_t reg, uint8_t value);
};

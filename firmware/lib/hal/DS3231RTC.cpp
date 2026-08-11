#include "DS3231RTC.h"

#include <sys/time.h>

uint8_t DS3231RTC::bcd2dec(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

uint8_t DS3231RTC::dec2bcd(uint8_t dec) {
    return ((dec / 10) << 4) | (dec % 10);
}

uint8_t DS3231RTC::readRegister(uint8_t reg) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    Wire.requestFrom(I2C_ADDR, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return 0;
}

void DS3231RTC::writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

bool DS3231RTC::detect() {
    Wire.beginTransmission(I2C_ADDR);
    return Wire.endTransmission() == 0;
}

time_t DS3231RTC::readTime() {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(0x00);  // 从寄存器 0 开始读取
    if (Wire.endTransmission(false) != 0) {
        Serial.printf("[%lu] [RTC] I2C read failed\n", millis());
        return 0;
    }

    Wire.requestFrom(I2C_ADDR, (uint8_t)7);
    if (Wire.available() < 7) {
        Serial.printf("[%lu] [RTC] Not enough data from RTC\n", millis());
        return 0;
    }

    uint8_t seconds = bcd2dec(Wire.read() & 0x7F);  // 去掉 CH 位
    uint8_t minutes = bcd2dec(Wire.read());
    uint8_t hours   = bcd2dec(Wire.read() & 0x3F);  // 24 小时制
    Wire.read();  // 跳过星期
    uint8_t day     = bcd2dec(Wire.read());
    uint8_t month   = bcd2dec(Wire.read() & 0x1F);  // 去掉世纪位
    uint8_t year    = bcd2dec(Wire.read());

    struct tm t = {};
    t.tm_sec  = seconds;
    t.tm_min  = minutes;
    t.tm_hour = hours;
    t.tm_mday = day;
    t.tm_mon  = month - 1;  // tm_mon 从 0 开始
    t.tm_year = year + 100; // tm_year 从 1900 开始，DS3231 年份从 2000 开始
    
    // DS3231 RTC 存储的是本地时间（北京时间），不是 UTC
    // 直接使用系统时区（CST-8）解释 struct tm
    // mktime 会根据当前 TZ 环境变量将本地时间转换为 UTC 时间戳
    time_t result = mktime(&t);

    Serial.printf("[%lu] [RTC] Read: %04d-%02d-%02d %02d:%02d:%02d (unix=%ld)\n",
                  millis(), year + 2000, month, day, hours, minutes, seconds, (long)result);

    // 基本合理性检查：时间应该在 2024 年之后
    if (result < 1704067200) {  // 2024-01-01 00:00:00 UTC
        Serial.printf("[%lu] [RTC] Time seems invalid (before 2024), ignoring\n", millis());
        return 0;
    }

    return result;
}

bool DS3231RTC::writeTime(time_t unixTime) {
    struct tm t;
    // 使用 localtime_r 将 UTC 时间戳转换为本地时间（北京时间）
    // 因为 RTC 存储的是本地时间，不是 UTC
    localtime_r(&unixTime, &t);

    Wire.beginTransmission(I2C_ADDR);
    Wire.write(0x00);  // 从寄存器 0 开始写入
    Wire.write(dec2bcd(t.tm_sec));
    Wire.write(dec2bcd(t.tm_min));
    Wire.write(dec2bcd(t.tm_hour));
    Wire.write(dec2bcd(t.tm_wday + 1));  // DS3231 星期从 1 开始
    Wire.write(dec2bcd(t.tm_mday));
    Wire.write(dec2bcd(t.tm_mon + 1));   // DS3231 月份从 1 开始
    Wire.write(dec2bcd(t.tm_year - 100)); // DS3231 年份从 2000 开始
    uint8_t err = Wire.endTransmission();

    if (err != 0) {
        Serial.printf("[%lu] [RTC] Write failed (err=%d)\n", millis(), err);
        return false;
    }

    Serial.printf("[%lu] [RTC] Written: %04d-%02d-%02d %02d:%02d:%02d\n",
                  millis(), t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

bool DS3231RTC::syncSystemFromRTC() {
    if (!detect()) {
        Serial.printf("[%lu] [RTC] DS3231 not detected at 0x%02X\n", millis(), I2C_ADDR);
        return false;
    }

    time_t rtcTime = readTime();
    if (rtcTime == 0) {
        return false;
    }

    // 设置 ESP32 系统时钟
    struct timeval tv = { .tv_sec = rtcTime, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    Serial.printf("[%lu] [RTC] System clock set from RTC\n", millis());
    return true;
}

bool DS3231RTC::syncRTCFromSystem() {
    if (!detect()) {
        Serial.printf("[%lu] [RTC] DS3231 not detected at 0x%02X\n", millis(), I2C_ADDR);
        return false;
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);

    // 检查系统时间是否有效（NTP 同步后应该在 2024 年之后）
    if (tv.tv_sec < 1704067200) {
        Serial.printf("[%lu] [RTC] System time invalid, skip writing to RTC\n", millis());
        return false;
    }

    return writeTime(tv.tv_sec);
}

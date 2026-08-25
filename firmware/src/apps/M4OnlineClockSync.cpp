#include "apps/M4OnlineClockSync.h"

#include "apps/M4ClockPolicy.h"
#include "apps/providers/M4NativeWifi.h"

#include <Arduino.h>
#include <atomic>
#include <ctime>
#include <esp_sntp.h>

#ifdef CROSSPOINT_X3
#include "DS3231RTC.h"
#endif

namespace M4OnlineClockSync {
namespace {

constexpr uint32_t kTimeoutMs = 4000;
std::atomic<bool> gAttempted{false};

}  // namespace

void ensureOnce() {
  const time_t now = time(nullptr);
  if (!M4ClockPolicy::shouldAttemptOnlineSync(static_cast<std::int64_t>(now),
                                              gAttempted.load(std::memory_order_acquire))) {
    return;
  }
  if (!M4NativeWifi::isReady()) {
    Serial.printf("[Clock] skip SNTP (wifi not ready, now=%ld)\n", static_cast<long>(now));
    return;
  }
  gAttempted.store(true, std::memory_order_release);

  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.cloudflare.com");
  esp_sntp_init();

  const uint32_t t0 = millis();
  bool ok = false;
  while (millis() - t0 < kTimeoutMs) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      ok = true;
      break;
    }
    delay(50);
  }
  esp_sntp_stop();

  if (!ok) {
    Serial.printf("[Clock] SNTP timeout after %ums (now=%ld)\n", static_cast<unsigned>(millis() - t0),
                  static_cast<long>(time(nullptr)));
    return;
  }

  setenv("TZ", "CST-8", 1);
  tzset();
  const time_t synced = time(nullptr);
  struct tm tm {};
  localtime_r(&synced, &tm);
  Serial.printf("[Clock] SNTP ok %04d-%02d-%02d %02d:%02d:%02d unix=%ld\n", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<long>(synced));
#ifdef CROSSPOINT_X3
  if (DS3231RTC::syncRTCFromSystem()) {
    Serial.printf("[Clock] RTC written from SNTP\n");
  }
#endif
}

}  // namespace M4OnlineClockSync

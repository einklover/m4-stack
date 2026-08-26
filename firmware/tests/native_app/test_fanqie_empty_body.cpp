// Host regression for the Fanqie HTTP-200-empty-body fix (no SD/network).
// Mirrors two provider-scoped contracts so a future refactor cannot silently
// regress them:
//   1. M4NativeProviderHttp::canRetryZeroByteTransport accepts a 2xx response
//      with zero body bytes ("http_2xx_empty") for exactly one clean GET
//      retry, while POST and any response that already delivered body bytes
//      are never retried.
//   2. FanqieProvider::chapterDateUtc picks nt= as RTC first, then the
//      firmware build date, then the pinned last-resort constant.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

namespace {

// Mirrors M4NativeProviderHttp::Result fields used by the retry decision.
struct HttpResult {
  bool ok = false;
  int status = 0;
  size_t bytes = 0;
  std::string error;
};

// Faithful copy of M4NativeProviderHttp::canRetryZeroByteTransport.
bool canRetryZeroByteGet(const HttpResult& result, const std::string& method) {
  if (result.ok || result.bytes != 0 || result.error.empty()) return false;
  if (!method.empty() && method != "GET") return false;
  if (result.error == "http_2xx_empty") return true;
  if (result.status != 0) return false;

  const std::string& e = result.error;
  if (e.rfind("http_ESP_ERR_HTTP_", 0) == 0) {
    if (e.find("INVALID_ARG") != std::string::npos ||
        e.find("INVALID_STATE") != std::string::npos ||
        e.find("MAX_REDIRECT") != std::string::npos) {
      return false;
    }
    return true;
  }
  return e == "http_ESP_ERR_TCP_TRANSPORT_CONNECTION_FAILED" ||
         e == "http_ESP_ERR_TCP_TRANSPORT_CONNECTION_CLOSED";
}

// Injected-clock copy of FanqieProvider::chapterDateUtc.
std::string fanqieNtDate(long long nowSec, const char* buildDate) {
  // 40 >= worst-case snprintf output so -Wformat-truncation stays silent.
  char ymd[40] = {};
  if (nowSec >= 1700000000LL) {
    const time_t now = static_cast<time_t>(nowSec);
    struct tm tm {};
    if (gmtime_r(&now, &tm) != nullptr && (tm.tm_year + 1900) >= 2024) {
      std::snprintf(ymd, sizeof(ymd), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                    tm.tm_mday);
      return ymd;
    }
  }
  int day = 0, year = 0;
  char mon[8] = {};
  if (std::sscanf(buildDate, "%7s %d %d", mon, &day, &year) == 3 && year >= 2024 && day >= 1 &&
      day <= 31) {
    static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* hit = std::strstr(kMonths, mon);
    const int mi = hit ? static_cast<int>(hit - kMonths) / 3 + 1 : 1;
    std::snprintf(ymd, sizeof(ymd), "%04d-%02d-%02d", year, mi, day);
    return ymd;
  }
  return "2026-08-18";
}

void retryContract() {
  // 2xx + zero bytes classifies transiently and retries exactly for GET.
  assert(canRetryZeroByteGet(HttpResult{false, 200, 0, "http_2xx_empty"}, "GET"));
  assert(!canRetryZeroByteGet(HttpResult{false, 200, 0, "http_2xx_empty"}, "POST"));

  // Any body byte already reached the sink: never rewind, never retry.
  assert(!canRetryZeroByteGet(HttpResult{false, 200, 1, "http_2xx_empty"}, "GET"));

  // Non-empty 2xx success and ordinary HTTP status errors stay terminal.
  assert(!canRetryZeroByteGet(HttpResult{true, 200, 128, ""}, "GET"));
  assert(!canRetryZeroByteGet(HttpResult{false, 404, 0, "http_404"}, "GET"));
  assert(!canRetryZeroByteGet(HttpResult{false, 500, 0, "http_500"}, "GET"));

  // Pre-existing transport-error retries are preserved.
  assert(canRetryZeroByteGet(HttpResult{false, 0, 0, "http_ESP_ERR_HTTP_FETCH_HEADER"}, "GET"));
  assert(!canRetryZeroByteGet(HttpResult{false, 0, 0, "http_ESP_ERR_HTTP_INVALID_ARG"}, "GET"));
  assert(
      canRetryZeroByteGet(HttpResult{false, 0, 0, "http_ESP_ERR_TCP_TRANSPORT_CONNECTION_CLOSED"},
                          "GET"));
}

void dateFallbackOrder() {
  // Sane RTC wins over the build date.
  assert(fanqieNtDate(1735689600LL, "Aug 24 2026") == "2025-01-01");

  // Invalid clock falls back to the firmware build date.
  assert(fanqieNtDate(0, "Aug 24 2026") == "2026-08-24");
  assert(fanqieNtDate(12345, "Jul  9 2025") == "2025-07-09");

  // Unusable build date falls back to the pinned constant.
  assert(fanqieNtDate(0, "") == "2026-08-18");
}

}  // namespace

int main() {
  retryContract();
  dateFallbackOrder();
  std::cout << "fanqie empty-body / nt-date contract tests passed\n";
  return 0;
}

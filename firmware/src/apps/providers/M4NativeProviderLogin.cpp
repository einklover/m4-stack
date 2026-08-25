#include "apps/providers/M4NativeProviderLogin.h"

#include "apps/providers/M4JjwxcEndpoint.h"
#include "apps/providers/M4WereadEndpoint.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4Psram.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeWifi.h"
#include "apps/M4OnlineClockSync.h"
#include "util/M4WereadAuthPolicy.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace M4NativeProviderLogin {
namespace {

struct SmallResponse {
  bool ok = false;
  int status = 0;
  std::string body;
  std::vector<std::string> setCookies;
  std::string error;
};

struct ResponseCtx {
  std::string body;
  std::vector<std::string> setCookies;
  size_t cap = 32u * 1024u;
  bool overflow = false;
};

std::mutex gMu;
Snapshot gSnapshot;
std::atomic<bool> gCancel{false};
std::atomic<bool> gBusy{false};
TaskHandle_t gTask = nullptr;

void publish(Phase phase, const char* status = nullptr, const char* error = nullptr,
             const std::string* qr = nullptr) {
  std::lock_guard<std::mutex> lock(gMu);
  gSnapshot.phase = phase;
  if (status) gSnapshot.status = status;
  if (error) gSnapshot.error = error;
  if (qr) gSnapshot.qrUrl = *qr;
  gSnapshot.updatedMs = millis();
}

bool cancelled() { return gCancel.load(std::memory_order_acquire); }

esp_err_t httpEvent(esp_http_client_event_t* evt) {
  auto* ctx = static_cast<ResponseCtx*>(evt->user_data);
  if (!ctx) return ESP_OK;
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
    const size_t n = static_cast<size_t>(evt->data_len);
    if (ctx->body.size() > ctx->cap || n > ctx->cap - ctx->body.size()) {
      ctx->overflow = true;
      return ESP_FAIL;
    }
    ctx->body.append(static_cast<const char*>(evt->data), n);
  } else if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
    std::string key = evt->header_key;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (key == "set-cookie") ctx->setCookies.emplace_back(evt->header_value);
  }
  return ESP_OK;
}

SmallResponse smallRequest(const char* method, const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers = {},
                           const std::string& body = {}, size_t cap = 32u * 1024u,
                           bool honorCancel = true) {
  SmallResponse out;
  if (honorCancel && cancelled()) {
    out.error = "cancelled";
    return out;
  }
  const auto wifi = M4NativeWifi::ensureConnected(20000, [honorCancel] {
    return honorCancel && cancelled();
  });
  if (!wifi.ok) {
    out.error = wifi.error.empty() ? "wifi_connect_failed" : wifi.error;
    return out;
  }
  if (honorCancel && cancelled()) {
    out.error = "cancelled";
    return out;
  }
  M4OnlineClockSync::ensureOnce();

  // Lock only the actual HTTP/TLS transaction. The QR polling loop releases
  // this gate during its 2-second wait, so a login screen never monopolizes
  // the reader for 90-120 seconds. Chapter TLS/decode and login TLS therefore
  // cannot overlap their internal-RAM peaks.
  M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
  if (honorCancel && cancelled()) {
    out.error = "cancelled";
    return out;
  }
  if (url.compare(0, 8, "https://") == 0 &&
      !M4NativeProviderHeavyGate::tlsBlockAvailable()) {
    out.error = "tls_internal_oom";
    return out;
  }

  ResponseCtx ctx;
  ctx.cap = cap;
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.event_handler = httpEvent;
  cfg.user_data = &ctx;
  cfg.timeout_ms = 20000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.disable_auto_redirect = false;

  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) {
    out.error = "http_init_failed";
    return out;
  }
  if (std::strcmp(method, "POST") == 0) esp_http_client_set_method(h, HTTP_METHOD_POST);
  else esp_http_client_set_method(h, HTTP_METHOD_GET);
  esp_http_client_set_header(h, "User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1");
  esp_http_client_set_header(h, "Accept-Encoding", "identity");
  for (const auto& kv : headers) {
    if (!kv.first.empty()) esp_http_client_set_header(h, kv.first.c_str(), kv.second.c_str());
  }
  if (!body.empty()) esp_http_client_set_post_field(h, body.data(), static_cast<int>(body.size()));

  const esp_err_t err = esp_http_client_perform(h);
  out.status = esp_http_client_get_status_code(h);
  esp_http_client_cleanup(h);
  out.body = std::move(ctx.body);
  out.setCookies = std::move(ctx.setCookies);
  if (ctx.overflow) {
    out.error = "response_too_large";
    return out;
  }
  if (err != ESP_OK) {
    out.error = (honorCancel && cancelled()) ? "cancelled" : "http_request_failed";
    return out;
  }
  if (out.status < 200 || out.status >= 300) {
    out.error = std::string("http_") + std::to_string(out.status);
    return out;
  }
  out.ok = true;
  return out;
}

std::string jsonString(JsonVariantConst v) {
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.is<long long>()) return std::to_string(v.as<long long>());
  if (v.is<unsigned long long>()) return std::to_string(v.as<unsigned long long>());
  if (v.is<double>()) return std::to_string(static_cast<long long>(v.as<double>()));
  return {};
}

JsonVariantConst loginField(const JsonDocument& doc, const char* key) {
  JsonObjectConst data = doc["data"].as<JsonObjectConst>();
  if (!data.isNull()) {
    JsonVariantConst v = data[key];
    if (!v.isNull()) return v;
  }
  return doc[key];
}

bool waitCancelable(uint32_t ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (cancelled()) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

bool tryRenewWeread(const std::string& root, bool honorCancel) {
  std::string cookie;
  if (!M4NativeProviderIo::loadCookieHeader(root, "weread", cookie)) return false;
  if (cookie.find("wr_rt=") == std::string::npos) return false;
  SmallResponse r = smallRequest(
      "POST", std::string(M4_WEREAD_ORIGIN) + M4WereadAuthPolicy::kRenewalPath,
      {{"Cookie", cookie},
       {"Referer", "https://weread.qq.com/"},
       {"Content-Type", "application/json"}},
      M4WereadAuthPolicy::kRenewalBody, 8u * 1024u, honorCancel);
  if (!r.setCookies.empty()) {
    (void)M4NativeProviderIo::mergeSetCookies(root, "weread", r.setCookies);
  }
  if (!r.ok) return false;
  if (M4WereadAuthPolicy::responseIndicatesLoginRequired(r.body)) return false;
  return M4NativeProviderIo::hasCredential(root, "weread");
}

bool wereadLogin(const std::string& root) {
  publish(Phase::Connecting, "连接微信读书…");
  SmallResponse uidResp = smallRequest("GET", std::string(M4_WEREAD_ORIGIN) + "/api/auth/getLoginUid");
  if (!uidResp.ok) {
    publish(uidResp.error == "cancelled" ? Phase::Cancelled : Phase::Error,
            nullptr, uidResp.error.c_str());
    return false;
  }
  if (!uidResp.setCookies.empty()) (void)M4NativeProviderIo::mergeSetCookies(root, "weread", uidResp.setCookies);

  JsonDocument uidDoc;
  if (deserializeJson(uidDoc, uidResp.body)) {
    publish(Phase::Error, nullptr, "login_uid_json");
    return false;
  }
  std::string uid = jsonString(uidDoc["uid"]);
  if (uid.empty()) uid = jsonString(uidDoc["data"]["uid"]);
  if (uid.empty() || uid.size() > 128) {
    publish(Phase::Error, nullptr, "login_uid_missing");
    return false;
  }

  const std::string qr = std::string(M4_WEREAD_PUBLIC_ORIGIN) + "/web/confirm?uid=" + uid;
  publish(Phase::WaitingScan, "请用微信或微信读书扫码", nullptr, &qr);

  const uint32_t started = millis();
  while (millis() - started < 120000u) {
    if (!waitCancelable(2000)) {
      publish(Phase::Cancelled, "已取消");
      return false;
    }
    SmallResponse poll = smallRequest(
        "GET", std::string(M4_WEREAD_ORIGIN) + "/api/auth/getLoginInfo?uid=" + uid + "&otp=");
    if (cancelled()) {
      publish(Phase::Cancelled, "已取消");
      return false;
    }
    if (!poll.ok) continue;  // transient poll failure; keep QR usable
    if (!poll.setCookies.empty()) (void)M4NativeProviderIo::mergeSetCookies(root, "weread", poll.setCookies);

    JsonDocument doc;
    if (deserializeJson(doc, poll.body)) continue;
    const bool succeed = loginField(doc, "succeed").as<bool>();
    const std::string logic = jsonString(loginField(doc, "logicCode"));
    if (logic == "NEED_OTP" || logic == "OTP_EXPIRED" || logic == "OTP_NOT_MATCH") {
      publish(Phase::Error, nullptr, "otp_required");
      return false;
    }
    if (!logic.empty() && logic != "LOGIN_TIMEOUT" && !succeed) {
      publish(Phase::Error, nullptr, logic.c_str());
      return false;
    }
    if (!succeed) continue;

    std::string vid = jsonString(loginField(doc, "webLoginVid"));
    if (vid.empty()) vid = jsonString(loginField(doc, "vid"));
    if (vid.empty()) vid = jsonString(loginField(doc, "userVid"));
    if (vid.empty()) vid = jsonString(loginField(doc, "user_vid"));
    const std::string token = jsonString(loginField(doc, "accessToken"));
    std::string rt = jsonString(loginField(doc, "refreshToken"));
    if (rt.empty()) rt = jsonString(loginField(doc, "wr_rt"));

    if (!M4NativeProviderIo::hasCredential(root, "weread")) {
      std::vector<std::pair<std::string, std::string>> fallback;
      if (!vid.empty()) fallback.push_back({"wr_vid", vid});
      if (!token.empty()) fallback.push_back({"wr_skey", token});
      if (!rt.empty()) fallback.push_back({"wr_rt", rt});
      if (!fallback.empty()) (void)M4NativeProviderIo::storeCookieValues(root, fallback);
    }
    if (M4NativeProviderIo::hasCredential(root, "weread")) {
      (void)tryRenewWeread(root, true);
      publish(Phase::Success, "登录成功");
      return true;
    }
    publish(Phase::Error, nullptr, "login_cookie_missing");
    return false;
  }
  publish(Phase::Error, nullptr, "login_timeout");
  return false;
}

std::string extractJjKey(const std::string& html) {
  static const char* needles[] = {"jjreaderKey = \"", "jjreaderKey=\"", "jjreaderKey = '\\'", "jjreaderKey='"};
  for (const char* needle : needles) {
    const size_t p = html.find(needle);
    if (p == std::string::npos) continue;
    const size_t start = p + std::strlen(needle);
    const char quote = needle[std::strlen(needle) - 1];
    const size_t end = html.find(quote, start);
    if (end == std::string::npos || end <= start || end - start > 128) continue;
    return html.substr(start, end - start);
  }
  return {};
}

bool jjwxcLogin(const std::string& root) {
  publish(Phase::Connecting, "连接晋江登录…");
  const std::string loginUrl = std::string(M4_JJWXC_MY_BASE) + "/backend/login/jjreader/login.php";
  SmallResponse page = smallRequest("GET", loginUrl, {}, {}, 64u * 1024u);
  if (!page.ok) {
    publish(page.error == "cancelled" ? Phase::Cancelled : Phase::Error,
            nullptr, page.error.c_str());
    return false;
  }
  if (!page.setCookies.empty()) (void)M4NativeProviderIo::mergeSetCookies(root, "jjwxc", page.setCookies);
  const std::string key = extractJjKey(page.body);
  if (key.empty()) {
    publish(Phase::Error, nullptr, "jjreader_key_missing");
    return false;
  }
  const std::string qr = loginUrl + "?sign=" + key;
  publish(Phase::WaitingScan, "请用晋江 App 扫码", nullptr, &qr);

  const std::string callback = std::string(M4_JJWXC_MY_BASE) + "/backend/login/jjreader/callback.php";
  const uint32_t started = millis();
  while (millis() - started < 90000u) {
    if (!waitCancelable(2000)) {
      publish(Phase::Cancelled, "已取消");
      return false;
    }
    SmallResponse check = smallRequest(
        "POST", callback,
        {{"Content-Type", "application/x-www-form-urlencoded"}, {"Referer", loginUrl}},
        std::string("jjreaderKey=") + key + "&action=check");
    if (!check.ok) continue;
    if (!check.setCookies.empty()) (void)M4NativeProviderIo::mergeSetCookies(root, "jjwxc", check.setCookies);
    JsonDocument doc;
    if (deserializeJson(doc, check.body)) continue;
    const int status = doc["status"] | 0;
    if (status != 200) continue;

    SmallResponse finish = smallRequest(
        "GET", callback + "?action=login&jjreaderKey=" + key,
        {{"Referer", loginUrl}});
    if (!finish.ok) {
      publish(Phase::Error, nullptr, finish.error.c_str());
      return false;
    }
    if (!finish.setCookies.empty()) (void)M4NativeProviderIo::mergeSetCookies(root, "jjwxc", finish.setCookies);
    if (M4NativeProviderIo::hasCredential(root, "jjwxc")) {
      publish(Phase::Success, "登录成功");
      return true;
    }
    publish(Phase::Error, nullptr, "login_cookie_missing");
    return false;
  }
  publish(Phase::Error, nullptr, "login_timeout");
  return false;
}

void taskMain(void*) {
  Snapshot initial;
  {
    std::lock_guard<std::mutex> lock(gMu);
    initial = gSnapshot;
  }
  bool ok = false;
  if (initial.providerId == "weread") ok = wereadLogin(initial.appDataRoot);
  else if (initial.providerId == "jjwxc") ok = jjwxcLogin(initial.appDataRoot);
  else publish(Phase::Error, nullptr, "login_not_supported");
  (void)ok;
  gBusy.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(gMu);
    gTask = nullptr;
  }
  M4Psram::deleteTask(nullptr);
}

}  // namespace

bool start(const std::string& providerId, const std::string& appDataRoot) {
  if (providerId != "weread" && providerId != "jjwxc") return false;
  bool expected = false;
  if (!gBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
  gCancel.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(gMu);
    gSnapshot = {};
    gSnapshot.phase = Phase::Connecting;
    gSnapshot.providerId = providerId;
    gSnapshot.appDataRoot = appDataRoot;
    gSnapshot.status = "准备登录…";
    gSnapshot.startedMs = millis();
    gSnapshot.updatedMs = gSnapshot.startedMs;
  }
  TaskHandle_t handle = nullptr;
  // Stack in PSRAM (24KB) so login TLS leaves internal RAM for handshake.
  if (M4Psram::createTask(taskMain, "NativeLogin", 24u * 1024u, nullptr, 1, &handle) != pdPASS) {
    gBusy.store(false, std::memory_order_release);
    publish(Phase::Error, nullptr, "login_task_create");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(gMu);
    gTask = handle;
  }
  return true;
}

Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(gMu);
  return gSnapshot;
}

void cancel() { gCancel.store(true, std::memory_order_release); }

bool busy() { return gBusy.load(std::memory_order_acquire); }

bool tryRenewSession(const std::string& appDataRoot) {
  return tryRenewWeread(appDataRoot, false);
}

}  // namespace M4NativeProviderLogin

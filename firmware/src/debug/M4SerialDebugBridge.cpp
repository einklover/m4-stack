#if defined(CROSSPOINT_MURPHY_M4)

#include "debug/M4SerialDebugBridge.h"

#include "debug/M4WaveformLab.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>
#include <functional>

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "apps/M4HttpTransportProbe.h"
#include "apps/M4xInstaller.h"
#include "apps/M4xManifest.h"
#include "apps/M4xPaths.h"
#include "apps/M4xRegistry.h"
#include "apps/M4xWifiConnect.h"
#include "qemu/M4QemuNet.h"

namespace M4SerialDebug {
namespace {

bool parseKeyName(const char* name, MappedInputManager::Button& out) {
  if (!name) return false;
  struct Pair {
    const char* n;
    MappedInputManager::Button b;
  };
  static const Pair kMap[] = {
      {"Back", MappedInputManager::Button::Back},
      {"back", MappedInputManager::Button::Back},
      {"Confirm", MappedInputManager::Button::Confirm},
      {"confirm", MappedInputManager::Button::Confirm},
      {"Left", MappedInputManager::Button::Left},
      {"left", MappedInputManager::Button::Left},
      {"Right", MappedInputManager::Button::Right},
      {"right", MappedInputManager::Button::Right},
      {"Up", MappedInputManager::Button::Up},
      {"up", MappedInputManager::Button::Up},
      {"Down", MappedInputManager::Button::Down},
      {"down", MappedInputManager::Button::Down},
      // Murphy M4 side body: KEY_LOCK / power (GPIO0). Short-press policy is
      // SETTINGS.shortPwrBtn (full refresh / confirm / ignore / …).
      {"Power", MappedInputManager::Button::Power},
      {"power", MappedInputManager::Button::Power},
      {"PageBack", MappedInputManager::Button::PageBack},
      {"page_back", MappedInputManager::Button::PageBack},
      {"PageForward", MappedInputManager::Button::PageForward},
      {"page_forward", MappedInputManager::Button::PageForward},
  };
  for (const auto& p : kMap) {
    if (strcmp(name, p.n) == 0) {
      out = p.b;
      return true;
    }
  }
  return false;
}

// SSIDs are user-controlled text.  The bridge protocol carries JSON, so keep
// status frames valid even for an SSID containing quotes, backslashes, or
// control bytes.  (Passwords are never formatted here.)
void copyJsonSafe(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) return;
  size_t n = 0;
  if (in) {
    while (in[n] && n + 1 < cap) {
      const unsigned char c = static_cast<unsigned char>(in[n]);
      out[n] = (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) ? '_' : static_cast<char>(c);
      ++n;
    }
  }
  out[n] = 0;
}

void rotateToPhysical(GfxRenderer::Orientation o, int x, int y, int* phyX, int* phyY) {
  switch (o) {
    case GfxRenderer::Portrait:
      *phyX = y;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - x;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - y;
      *phyY = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
    default:
      *phyX = x;
      *phyY = y;
      break;
  }
}

// SdFat may legally complete a write partially (or report zero while the
// card is busy allocating a FAT cluster).  The old bridge treated either
// case as a permanent failure, which made otherwise healthy cards fail at a
// repeatable offset during Wi-Fi/USB plugin installs.  Keep retries bounded so
// a genuinely removed/full card still returns a deterministic error.
bool writeFileAll(FsFile& file, const uint8_t* data, size_t len) {
  if (!data && len != 0) return false;
  size_t off = 0;
  unsigned noProgress = 0;
  while (off < len) {
    const size_t n = file.write(data + off, len - off);
    if (n > 0) {
      off += n;
      noProgress = 0;
      continue;
    }
    if (++noProgress >= 4) return false;
    // Give SdFat/SDMMC time to finish an internal erase/cluster allocation.
    (void)file.sync();
    delay(2);
  }
  return true;
}

struct SdProbeResult {
  bool open = false;
  bool write = false;
  bool sync = false;
  bool read = false;
  bool remove = false;
  const char* failed = "";
};

SdProbeResult probeSdWritable() {
  SdProbeResult r;
  if (!SdMan.ready()) {
    r.failed = "not_ready";
    return r;
  }
  M4xInstaller::ensureLayout();
  constexpr char kPath[] = "/apps_inbox/.m4_sd_probe.tmp";
  if (SdMan.exists(kPath)) SdMan.remove(kPath);

  FsFile f;
  if (!SdMan.openFileForWrite("M4Probe", kPath, f)) {
    r.failed = "open";
    return r;
  }
  r.open = true;
  uint8_t pattern[512];
  for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = static_cast<uint8_t>(i ^ 0xA5u);
  for (unsigned i = 0; i < 8; ++i) {
    if (!writeFileAll(f, pattern, sizeof(pattern))) {
      f.close();
      SdMan.remove(kPath);
      r.failed = "write";
      return r;
    }
  }
  r.write = true;
  r.sync = f.sync();
  f.close();
  if (!r.sync) {
    SdMan.remove(kPath);
    r.failed = "sync";
    return r;
  }

  FsFile in;
  if (!SdMan.openFileForRead("M4Probe", kPath, in)) {
    SdMan.remove(kPath);
    r.failed = "read_open";
    return r;
  }
  r.read = true;
  uint8_t got[512];
  for (unsigned i = 0; i < 8; ++i) {
    const int n = in.read(got, sizeof(got));
    if (n != static_cast<int>(sizeof(got)) || memcmp(got, pattern, sizeof(got)) != 0) {
      r.read = false;
      break;
    }
  }
  in.close();
  if (!r.read) r.failed = "read_verify";
  r.remove = SdMan.remove(kPath);
  if (!r.remove && !r.failed[0]) r.failed = "remove";
  return r;
}

constexpr size_t kScreenshotRawChunk = 384;  // 512 B base64 line; safer on HWCDC

// Defined with the Wi-Fi radio adapter below.  The request dispatcher uses
// the same bounded helper for wifi_prepare and install_http.
bool ensureStaConnected(uint32_t timeoutMs);

}  // namespace

void Bridge::begin(GfxRenderer* renderer, MappedInputManager* input, HalDisplay* display, HostHooks hooks) {
  renderer_ = renderer;
  input_ = input;
  hooks_ = std::move(hooks);
  auth_ = {};
  intake_.reset();
  intake_.discardUntilNewline = false;
  // Waveform Lab (hidden USB feature) uses the live panel driver.
  M4WaveformLab::setDisplay(display);
  // Bridge compiled in; remains unauthorized until Developer Options enables it.
  Serial.printf("[%lu] [M4DBG] serial debug bridge present protocol=%d (auth=off)\n", millis(), kProtocolVersion);
}

void Bridge::clearIdempotency() {
  for (size_t i = 0; i < kIdempotencySlots; ++i) {
    idem_[i].used = false;
    idem_[i].id[0] = 0;
  }
  idemNext_ = 0;
}

void Bridge::resetSessionState(bool removePart) {
  abortUpload(removePart);
  shotActive_ = false;
  shotReqId_[0] = 0;
  shotOffset_ = 0;
  shotTotal_ = 0;
  if (shaReady_) {
    mbedtls_sha256_free(reinterpret_cast<mbedtls_sha256_context*>(shaCtx_));
    shaReady_ = false;
  }
  clearIdempotency();
  intake_.reset();
  intake_.discardUntilNewline = false;
  lastHostActivityMs_ = 0;
  lastChunkAckJson_[0] = 0;
}

void Bridge::flushRxDiscard() {
  int budget = 256;
  while (budget-- > 0 && Serial.available() > 0) {
    (void)Serial.read();
  }
  intake_.reset();
  intake_.discardUntilNewline = false;
}

void Bridge::setAuthorized(bool desiredAuthorized) {
  const auto tr = auth_.applyDesired(desiredAuthorized);
  if (tr == M4SerialDebugPolicy::AuthTransition::None) return;
  if (tr == M4SerialDebugPolicy::AuthTransition::Enable) {
    // Do not execute immediately. poll() drains in bounded slices until CDC RX
    // is genuinely empty, so a backlog larger than one slice cannot survive.
    enableRxDrainPending_ = true;
    intake_.reset();
    intake_.discardUntilNewline = false;
    clearIdempotency();
    Serial.printf("[%lu] [M4DBG] USB serial debugging ENABLED (local UI)\n", millis());
    return;
  }
  // Disable: abort in-flight upload/screenshot and forget host keep-awake.
  enableRxDrainPending_ = false;
  resetSessionState(true);
  flushRxDiscard();
  Serial.printf("[%lu] [M4DBG] USB serial debugging DISABLED (local UI)\n", millis());
}

void Bridge::noteHostActivity() {
  if (!auth_.authorized) return;
  lastHostActivityMs_ = millis();
}

bool Bridge::recentHostActivity(unsigned long nowMs, unsigned long windowMs) const {
  return auth_.shouldKeepAwake(nowMs, lastHostActivityMs_, windowMs);
}

void Bridge::poll() {
  // Drain enough CDC RX per tick that a full @M4DBG control frame (~300–800 B)
  // is not stuck across many e-ink-blocked loops. Cap keeps the owner loop fair.
  constexpr int kRxBudget = 1024;
  if (!auth_.shouldExecuteFrames()) {
    // Boundedly discard CDC RX so the buffer cannot arm commands after enable.
    int budget = kRxBudget;
    while (budget-- > 0 && Serial.available() > 0) {
      (void)Serial.read();
    }
    intake_.reset();
    intake_.discardUntilNewline = false;
    return;
  }
  if (enableRxDrainPending_) {
    // Continue discarding across frames until empty. Never parse in the same
    // frame that observes empty: only bytes arriving afterwards are eligible.
    int budget = kRxBudget;
    while (budget-- > 0 && Serial.available() > 0) {
      (void)Serial.read();
    }
    intake_.reset();
    intake_.discardUntilNewline = false;
    if (Serial.available() == 0) {
      enableRxDrainPending_ = false;
    }
    return;
  }
  int budget = kRxBudget;
  while (budget-- > 0 && Serial.available() > 0) {
    const int b = Serial.read();
    if (b < 0) break;
    feedByte(static_cast<char>(b));
  }
  // Pump the async Waveform Lab animation one step per loop so the main
  // loop (and its watchdog) never blocks for the whole animation.
  if (M4WaveformLab::animateActive()) {
    uint32_t stepMs = 0;
    M4WaveformLab::pumpAnimateWindow(stepMs);
  }
  if (shotActive_) pollScreenshot();
}

void Bridge::feedByte(char c) {
  bool lineReady = false;
  intake_.feed(c, lineReady);
  if (!lineReady) return;
  // Complete line in intake_.buf; discard path never sets lineReady.
  handleLine(intake_.buf);
  intake_.clearAfterHandle();
}

bool Bridge::parseFrame(const char* line, char* reqIdOut, size_t reqIdCap, char* kindOut, size_t kindCap,
                        const char** payloadStart) {
  if (strncmp(line, kPrefix, strlen(kPrefix)) != 0) return false;
  const char* p = line + strlen(kPrefix);
  while (*p == ' ') ++p;
  if (!*p) return false;
  size_t i = 0;
  while (*p && *p != ' ' && i + 1 < reqIdCap) reqIdOut[i++] = *p++;
  reqIdOut[i] = 0;
  if (i == 0) return false;
  while (*p == ' ') ++p;
  i = 0;
  while (*p && *p != ' ' && i + 1 < kindCap) kindOut[i++] = *p++;
  kindOut[i] = 0;
  if (i == 0) return false;
  while (*p == ' ') ++p;
  *payloadStart = p;
  return true;
}

void Bridge::handleLine(const char* line) {
  if (strncmp(line, kPrefix, strlen(kPrefix)) != 0) return;
  noteHostActivity();

  char reqId[kMaxReqIdLen + 1] = {};
  char kind[8] = {};
  const char* payload = nullptr;
  if (!parseFrame(line, reqId, sizeof(reqId), kind, sizeof(kind), &payload)) return;
  if (!M4SerialDebugPolicy::isValidReqId(reqId)) return;

  if (strcmp(kind, "req") == 0) {
    if (tryIdemReplay(reqId)) return;
    uint8_t decoded[kMaxJsonDecode];
    size_t decLen = 0;
    if (!payload || !*payload) {
      replyErr(reqId, "empty_payload", "请求体为空");
      return;
    }
    if (!M4SerialDebugPolicy::b64DecodeStrict(payload, strlen(payload), decoded, sizeof(decoded) - 1, decLen)) {
      replyErr(reqId, "b64_decode", "Base64 解码失败");
      return;
    }
    decoded[decLen] = 0;
    handleReq(reqId, reinterpret_cast<const char*>(decoded), decLen);
    return;
  }

  if (strcmp(kind, "chk") == 0) {
    // Chunk acks are cached by req id for lost-ack retries.
    if (tryIdemReplay(reqId)) return;
    if (!payload) {
      replyErr(reqId, "bad_chunk", "分片格式错误");
      return;
    }
    char* end = nullptr;
    const unsigned long seq = strtoul(payload, &end, 10);
    if (!end || *end != ' ') {
      replyErr(reqId, "bad_chunk", "分片序号错误");
      return;
    }
    while (*end == ' ') ++end;
    const unsigned long total = strtoul(end, &end, 10);
    if (!end || *end != ' ') {
      replyErr(reqId, "bad_chunk", "分片总数错误");
      return;
    }
    while (*end == ' ') ++end;
    uint8_t raw[kMaxRawChunk + 8];
    size_t rawLen = 0;
    if (!M4SerialDebugPolicy::b64DecodeStrict(end, strlen(end), raw, sizeof(raw), rawLen) || rawLen == 0 ||
        rawLen > kMaxRawChunk) {
      replyErr(reqId, "bad_chunk", "分片数据无效或过大");
      return;
    }
    handleChk(reqId, static_cast<uint32_t>(seq), static_cast<uint32_t>(total), raw, rawLen);
  }
}

bool Bridge::tryIdemReplay(const char* reqId) {
  for (size_t i = 0; i < kIdempotencySlots; ++i) {
    if (idem_[i].used && strcmp(idem_[i].id, reqId) == 0) {
      Serial.printf("%s %s %s %s\n", kPrefix, reqId, idem_[i].kind, idem_[i].payloadB64);
      return true;
    }
  }
  return false;
}

void Bridge::storeIdem(const char* reqId, const char* kind, const char* payloadB64) {
  IdemSlot& s = idem_[idemNext_ % kIdempotencySlots];
  idemNext_++;
  s.used = true;
  strncpy(s.id, reqId, kMaxReqIdLen);
  s.id[kMaxReqIdLen] = 0;
  strncpy(s.kind, kind, sizeof(s.kind) - 1);
  s.kind[sizeof(s.kind) - 1] = 0;
  strncpy(s.payloadB64, payloadB64 ? payloadB64 : "", sizeof(s.payloadB64) - 1);
  s.payloadB64[sizeof(s.payloadB64) - 1] = 0;
}

void Bridge::replyOk(const char* reqId, const char* jsonObj, bool cacheIdem) {
  // Status/wifi responses can legitimately be larger than the original
  // 512-byte status object.  Keep the encoded frame below kMaxLineLen while
  // leaving headroom for SSIDs and diagnostics.
  char b64[1400];
  M4SerialDebugPolicy::b64Encode(reinterpret_cast<const uint8_t*>(jsonObj), strlen(jsonObj), b64, sizeof(b64));
  Serial.printf("%s %s ok %s\n", kPrefix, reqId, b64);
  if (cacheIdem) storeIdem(reqId, "ok", b64);
}

void Bridge::replyErr(const char* reqId, const char* key, const char* zhMsg, bool cacheIdem) {
  char json[240];
  snprintf(json, sizeof(json), "{\"error\":\"%s\",\"message\":\"%s\"}", key, zhMsg ? zhMsg : "");
  char b64[400];
  M4SerialDebugPolicy::b64Encode(reinterpret_cast<const uint8_t*>(json), strlen(json), b64, sizeof(b64));
  Serial.printf("%s %s err %s\n", kPrefix, reqId, b64);
  if (cacheIdem) storeIdem(reqId, "err", b64);
}

void Bridge::replyProgress(const char* reqId, const char* jsonObj) {
  if (!reqId || !jsonObj) return;
  char b64[500];
  M4SerialDebugPolicy::b64Encode(reinterpret_cast<const uint8_t*>(jsonObj), strlen(jsonObj), b64, sizeof(b64));
  Serial.printf("%s %s prg %s\n", kPrefix, reqId, b64);
  Serial.flush();
  noteHostActivity();
}

void Bridge::handleReq(const char* reqId, const char* json, size_t jsonLen) {
  JsonDocument doc;
  if (deserializeJson(doc, json, jsonLen)) {
    replyErr(reqId, "bad_json", "JSON 解析失败");
    return;
  }
  const char* op = doc["op"] | "";
  if (!op || !*op) {
    replyErr(reqId, "missing_op", "缺少 op 字段");
    return;
  }

  if (strcmp(op, "ping") == 0 || strcmp(op, "status") == 0) {
    StatusSnapshot st{};
    if (hooks_.status) st = hooks_.status();
    // Copy strings into durable buffers before snprintf.
    strncpy(activityCopy_, st.activity ? st.activity : "", sizeof(activityCopy_) - 1);
    activityCopy_[sizeof(activityCopy_) - 1] = 0;
    strncpy(appIdCopy_, st.activeAppId ? st.activeAppId : "", sizeof(appIdCopy_) - 1);
    appIdCopy_[sizeof(appIdCopy_) - 1] = 0;
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    const bool wifiConnected = m4QemuNetWifiCompatConnected() || WiFi.status() == WL_CONNECTED;
    char qemuIp[16] = {};
    if (m4QemuNetIsUp()) m4QemuNetLocalIp(qemuIp, sizeof(qemuIp));
    const String wifiSsid = m4QemuNetWifiCompatConnected()
                                ? String("qemu-openeth")
                                : (wifiConnected ? WiFi.SSID() : String());
    const String wifiIp = m4QemuNetIsUp() ? String(qemuIp)
                                          : (wifiConnected ? WiFi.localIP().toString() : String());
    const int wifiRssi = m4QemuNetWifiCompatConnected() ? 0 : (wifiConnected ? WiFi.RSSI() : -127);
#else
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    const String wifiSsid = wifiConnected ? WiFi.SSID() : String();
    const String wifiIp = wifiConnected ? WiFi.localIP().toString() : String();
    const int wifiRssi = wifiConnected ? WiFi.RSSI() : -127;
#endif
    char wifiSsidSafe[96] = {};
    char wifiIpSafe[24] = {};
    copyJsonSafe(wifiSsid.c_str(), wifiSsidSafe, sizeof(wifiSsidSafe));
    copyJsonSafe(wifiIp.c_str(), wifiIpSafe, sizeof(wifiIpSafe));
    char out[960];
    snprintf(out, sizeof(out),
             "{\"op\":\"%s\",\"protocol\":%d,\"firmware\":\"%s\",\"activity\":\"%s\","
             "\"active_app\":\"%s\",\"free_heap\":%u,\"min_free_heap\":%u,\"free_psram\":%u,"
             "\"reset_reason\":%u,"
             "\"sd_ok\":%s,\"screen_w\":%d,\"screen_h\":%d,\"orientation\":%d,"
             "\"wifi_connected\":%s,\"wifi_status\":%d,\"wifi_ssid\":\"%s\",\"wifi_ip\":\"%s\","
             "\"wifi_rssi\":%d,\"caps\":[\"install\",\"install_http\",\"wifi_status\",\"wifi_prepare\","
             "\"wifi_transfer\",\"sd_probe\",\"sd_read\",\"http_probe\",\"launch\",\"tap\",\"key\","
             "\"swipe\",\"screenshot\",\"logs\",\"ui\"]}",
             op, kProtocolVersion, st.firmwareVersion ? st.firmwareVersion : "", activityCopy_, appIdCopy_,
             static_cast<unsigned>(st.freeHeap), static_cast<unsigned>(st.minFreeHeap),
             static_cast<unsigned>(st.freePsram), static_cast<unsigned>(st.resetReason),
             st.sdOk ? "true" : "false", st.screenW, st.screenH,
             st.orientation, wifiConnected ? "true" : "false", static_cast<int>(WiFi.status()), wifiSsidSafe,
             wifiIpSafe, wifiRssi);
    replyOk(reqId, out);
    return;
  }

  // Structured on-screen / plugin state for automation — no OCR required.
  // Returns activity + active_app + nested ui dump (failed/error/screen/list).
  if (strcmp(op, "ui") == 0) {
    StatusSnapshot st{};
    if (hooks_.status) st = hooks_.status();
    strncpy(activityCopy_, st.activity ? st.activity : "", sizeof(activityCopy_) - 1);
    activityCopy_[sizeof(activityCopy_) - 1] = 0;
    strncpy(appIdCopy_, st.activeAppId ? st.activeAppId : "", sizeof(appIdCopy_) - 1);
    appIdCopy_[sizeof(appIdCopy_) - 1] = 0;
    std::string dump = "{}";
    if (hooks_.uiDump) {
      dump = hooks_.uiDump();
      if (dump.empty()) dump = "{}";
    }
    // replyOk b64 buffer is ~1400 chars → keep raw JSON well under ~1000 B.
    if (dump.size() > 850) {
      dump.resize(850);
      while (!dump.empty() && dump.back() != '}') dump.pop_back();
      if (dump.empty() || dump.back() != '}') dump += '}';
    }
    char head[200];
    snprintf(head, sizeof(head),
             "{\"op\":\"ui\",\"activity\":\"%s\",\"active_app\":\"%s\",\"ui\":", activityCopy_, appIdCopy_);
    std::string out = head;
    out += dump;
    out += '}';
    if (out.size() > 1000) out.resize(1000);
    replyOk(reqId, out.c_str());
    return;
  }

  auto wifiJson = [&](const char* opName, bool ready) {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    const bool connected = m4QemuNetWifiCompatConnected() || WiFi.status() == WL_CONNECTED;
    char qemuIp[16] = {};
    if (m4QemuNetIsUp()) m4QemuNetLocalIp(qemuIp, sizeof(qemuIp));
    const String ssid = m4QemuNetWifiCompatConnected() ? String("qemu-openeth")
                                                       : (connected ? WiFi.SSID() : String());
    const String ip = m4QemuNetIsUp() ? String(qemuIp)
                                     : (connected ? WiFi.localIP().toString() : String());
    const int rssi = m4QemuNetWifiCompatConnected() ? 0 : (connected ? WiFi.RSSI() : -127);
#else
    const bool connected = WiFi.status() == WL_CONNECTED;
    const String ssid = connected ? WiFi.SSID() : String();
    const String ip = connected ? WiFi.localIP().toString() : String();
    const int rssi = connected ? WiFi.RSSI() : -127;
#endif
    char ssidSafe[96] = {};
    char ipSafe[24] = {};
    copyJsonSafe(ssid.c_str(), ssidSafe, sizeof(ssidSafe));
    copyJsonSafe(ip.c_str(), ipSafe, sizeof(ipSafe));
    char out[420];
    snprintf(out, sizeof(out),
             "{\"op\":\"%s\",\"ready\":%s,\"connected\":%s,\"status\":%d,"
             "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"url\":\"%s\"}",
             opName, ready ? "true" : "false", connected ? "true" : "false", static_cast<int>(WiFi.status()),
             ssidSafe, ipSafe, rssi,
             (connected && ip.length() > 0) ? (String("http://") + ip + "/").c_str() : "");
    return std::string(out);
  };

  // Read-only status is useful before starting a transfer and never changes
  // credentials or radio mode.
  if (strcmp(op, "wifi_status") == 0) {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    const bool ready = m4QemuNetIsUp() ||
                       (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0));
#else
    const bool ready = WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
#endif
    const auto out = wifiJson("wifi_status", ready);
    replyOk(reqId, out.c_str());
    return;
  }

  // USB is the control plane: bring up the saved STA connection, disable
  // power-save latency, and return the device URL.  Package bytes still flow
  // over LAN in install_http; no credentials are returned on this channel.
  if (strcmp(op, "wifi_prepare") == 0 || strcmp(op, "wifi_transfer") == 0) {
    if (uploadActive_ || shotActive_) {
      replyErr(reqId, "busy", "设备忙，请稍后重试");
      return;
    }
    noteHostActivity();
    replyProgress(reqId, "{\"op\":\"wifi_prepare\",\"phase\":\"connecting\"}");
    const bool ready = ensureStaConnected(30000);
    if (!ready) {
      // wifi_transfer intentionally opens the normal touch flow when there
      // is no usable saved network, so the user can select one on-device.
      if (strcmp(op, "wifi_transfer") == 0 && hooks_.openFileTransferUi) hooks_.openFileTransferUi();
      const auto out = wifiJson(op, false);
      replyOk(reqId, out.c_str(), true);
      return;
    }
    const auto out = wifiJson(op, true);
    if (strcmp(op, "wifi_transfer") == 0 && hooks_.openFileTransferUi) hooks_.openFileTransferUi();
    replyOk(reqId, out.c_str(), true);
    return;
  }

  if (strcmp(op, "sd_probe") == 0) {
    const SdProbeResult p = probeSdWritable();
    char out[320];
    snprintf(out, sizeof(out),
             "{\"op\":\"sd_probe\",\"ok\":%s,\"open\":%s,\"write\":%s,\"sync\":%s,"
             "\"read\":%s,\"delete\":%s,\"failed\":\"%s\",\"total_bytes\":%llu,\"used_bytes\":%llu,"
             "\"fat\":%u}",
             (p.open && p.write && p.sync && p.read && p.remove) ? "true" : "false", p.open ? "true" : "false",
             p.write ? "true" : "false", p.sync ? "true" : "false", p.read ? "true" : "false",
             p.remove ? "true" : "false", p.failed ? p.failed : "", static_cast<unsigned long long>(SdMan.sdTotalBytes()),
             static_cast<unsigned long long>(SdMan.sdUsedBytes()), static_cast<unsigned>(SdMan.lastFatType()));
    replyOk(reqId, out, true);
    return;
  }

  // Isolated M4HttpTransport / WeRead step debugger. Blocks the owner loop
  // like wifi_prepare (TLS may take tens of seconds). Prefer step-by-step
  // from host rather than chaining many network steps in one request.
  if (strcmp(op, "http_probe") == 0) {
    if (uploadActive_ || shotActive_) {
      replyErr(reqId, "busy", "设备忙，请稍后重试");
      return;
    }
    noteHostActivity();
    const char* step = doc["step"] | "";
    if (!step || !step[0]) {
      replyErr(reqId, "missing_step",
               "需要 step: mem|debug_on|session_begin|tls_get|weread_psvts|weread_e0|...");
      return;
    }
    char phase[96];
    snprintf(phase, sizeof(phase), "{\"op\":\"http_probe\",\"phase\":\"run\",\"step\":\"%.40s\"}", step);
    replyProgress(reqId, phase);

    M4HttpTransportProbe::Args a;
    a.step = step;
    a.host = doc["host"] | "weread.qq.com";
    a.url = doc["url"] | "https://weread.qq.com/";
    a.useSession = doc["session"] | true;
    a.bookId = doc["bookId"] | "";
    a.chapterUid = doc["chapterUid"] | "";
    a.title = doc["title"] | "";
    a.psvts = doc["psvts"] | "";
    a.appId = doc["appId"] | "com.weread.client";
    a.wrVid = doc["wr_vid"] | "";
    a.wrSkey = doc["wr_skey"] | "";
    a.wrRt = doc["wr_rt"] | "";
    int to = doc["timeout_ms"] | 30000;
    // Worker full-chapter fetch needs a longer default wall.
    if (strcmp(step, "weread_worker_fetch") == 0 && (doc["timeout_ms"] | 0) == 0) to = 90000;
    if (to < 5000) to = 5000;
    if (to > 90000) to = 90000;
    a.timeoutMs = static_cast<uint32_t>(to);

    M4HttpTransportProbe::Result pr;
    const bool known = M4HttpTransportProbe::run(a, pr);
    if (!known) {
      replyErr(reqId, pr.error[0] ? pr.error : "unknown_step", "未知 http_probe step", true);
      return;
    }

    char stepSafe[28] = {};
    char errSafe[52] = {};
    char detSafe[100] = {};
    char psvSafe[84] = {};
    copyJsonSafe(pr.step, stepSafe, sizeof(stepSafe));
    copyJsonSafe(pr.error, errSafe, sizeof(errSafe));
    copyJsonSafe(pr.detail, detSafe, sizeof(detSafe));
    copyJsonSafe(pr.psvts, psvSafe, sizeof(psvSafe));
    char out[1000];
    snprintf(
        out, sizeof(out),
        "{\"op\":\"http_probe\",\"ok\":%s,\"step\":\"%s\",\"error\":\"%s\",\"status\":%d,"
        "\"bytes\":%u,\"session_open\":%s,\"detail\":\"%s\",\"psvts\":\"%s\","
        "\"before\":{\"heap\":%u,\"min_heap\":%u,\"psram\":%u,\"int\":%u,\"larg\":%u,\"tls\":%s},"
        "\"after\":{\"heap\":%u,\"min_heap\":%u,\"psram\":%u,\"int\":%u,\"larg\":%u,\"tls\":%s}}",
        pr.ok ? "true" : "false", stepSafe, errSafe, pr.status, static_cast<unsigned>(pr.bytes),
        pr.sessionOpen ? "true" : "false", detSafe, psvSafe,
        static_cast<unsigned>(pr.before.freeHeap), static_cast<unsigned>(pr.before.minFreeHeap),
        static_cast<unsigned>(pr.before.freePsram), static_cast<unsigned>(pr.before.freeInternal),
        static_cast<unsigned>(pr.before.largestInternal), pr.before.tlsGateOk ? "true" : "false",
        static_cast<unsigned>(pr.after.freeHeap), static_cast<unsigned>(pr.after.minFreeHeap),
        static_cast<unsigned>(pr.after.freePsram), static_cast<unsigned>(pr.after.freeInternal),
        static_cast<unsigned>(pr.after.largestInternal), pr.after.tlsGateOk ? "true" : "false");
    // Soft failure (ok:false) still returns ok frame so host can inspect fields.
    replyOk(reqId, out, true);
    return;
  }

  // Bounded SD text/file pull over USB (no Wi-Fi). Only /apps_data/** and
  // /apps_inbox/** — for plugin error.log and install diagnostics.
  if (strcmp(op, "sd_read") == 0) {
    const char* pathIn = doc["path"] | "";
    int offset = doc["offset"] | -1;  // -1 = read tail
    int maxn = doc["max"] | 400;
    if (maxn < 1) maxn = 1;
    if (maxn > 400) maxn = 400;  // keep reply under serial line budget
    if (!pathIn || !pathIn[0]) {
      replyErr(reqId, "bad_path", "缺少 path");
      return;
    }
    // Normalize: allow "apps_data/..." or "/apps_data/..."
    char abs[192];
    if (pathIn[0] == '/') {
      snprintf(abs, sizeof(abs), "%s", pathIn);
    } else {
      snprintf(abs, sizeof(abs), "/%s", pathIn);
    }
    // Reject traversal / absolute escapes outside allow-list.
    if (std::strstr(abs, "..") != nullptr) {
      replyErr(reqId, "bad_path", "路径非法");
      return;
    }
    const bool okRoot = (std::strncmp(abs, "/apps_data/", 11) == 0) ||
                        (std::strncmp(abs, "/apps_inbox/", 12) == 0) ||
                        (std::strcmp(abs, "/apps_data") == 0) || (std::strcmp(abs, "/apps_inbox") == 0);
    if (!okRoot) {
      replyErr(reqId, "forbidden", "仅允许 apps_data 或 apps_inbox");
      return;
    }
    if (!SdMan.ready()) {
      replyErr(reqId, "sd_not_ready", "SD 未就绪");
      return;
    }
    if (!SdMan.exists(abs)) {
      replyErr(reqId, "missing", "文件不存在");
      return;
    }
    FsFile f;
    if (!SdMan.openFileForRead("M4Dbg", abs, f)) {
      replyErr(reqId, "open_failed", "无法打开文件");
      return;
    }
    const size_t fsz = static_cast<size_t>(f.fileSize());
    size_t off = 0;
    if (offset < 0) {
      off = (fsz > static_cast<size_t>(maxn)) ? (fsz - static_cast<size_t>(maxn)) : 0;
    } else {
      off = static_cast<size_t>(offset);
      if (off > fsz) off = fsz;
    }
    size_t want = static_cast<size_t>(maxn);
    if (off + want > fsz) want = fsz - off;
    if (off > 0) f.seek(off);
    uint8_t raw[400];
    size_t got = 0;
    while (got < want) {
      const int n = f.read(raw + got, want - got);
      if (n <= 0) break;
      got += static_cast<size_t>(n);
    }
    f.close();
    char b64[560];
    M4SerialDebugPolicy::b64Encode(raw, got, b64, sizeof(b64));
    char pathSafe[160];
    copyJsonSafe(abs, pathSafe, sizeof(pathSafe));
    char out[900];
    snprintf(out, sizeof(out),
             "{\"op\":\"sd_read\",\"ok\":true,\"path\":\"%s\",\"size\":%u,\"offset\":%u,"
             "\"n\":%u,\"eof\":%s,\"data_b64\":\"%s\"}",
             pathSafe, static_cast<unsigned>(fsz), static_cast<unsigned>(off), static_cast<unsigned>(got),
             (off + got >= fsz) ? "true" : "false", b64);
    replyOk(reqId, out);
    return;
  }

  // --- Waveform Lab (hidden USB feature) ---
  if (strcmp(op, "lut_begin") == 0) {
    if (uploadActive_ || shotActive_) {
      replyErr(reqId, "busy", "设备忙，请稍后重试");
      return;
    }
    const int slot = doc["slot"] | 0;
    const uint32_t size = doc["size"] | 0;
    if (slot < 0 || slot > 1) {
      replyErr(reqId, "bad_slot", "槽位非法");
      return;
    }
    if (size != M4WaveformLab::kFrameBytes) {
      replyErr(reqId, "bad_size", "帧大小必须为 48000");
      return;
    }
    if (!M4WaveformLab::beginFrameUpload(slot)) {
      replyErr(reqId, "lab_oom", "PSRAM 帧缓存分配失败");
      return;
    }
    chunk_.begin(size);
    labFrameActive_ = true;
    labFrameSlot_ = slot;
    replyOk(reqId, "{\"op\":\"lut_begin\",\"ready\":true}", true);
    return;
  }
  if (strcmp(op, "lut_upload") == 0) {
    const char* b64 = doc["lut"] | "";
    const bool unlock = doc["unlock_voltages"] | false;
    uint8_t lut[M4WaveformLab::kLutBytes];
    size_t n = 0;
    if (!M4SerialDebugPolicy::b64DecodeStrict(b64, strlen(b64), lut, sizeof(lut), n) ||
        n != M4WaveformLab::kLutBytes) {
      replyErr(reqId, "bad_lut", "LUT 必须为 110 字节");
      return;
    }
    if (!M4WaveformLab::setLut(lut, n, unlock)) {
      replyErr(reqId, "lut_locked", "电压字节被锁定（unlock_voltages=true 才可改）");
      return;
    }
    replyOk(reqId, "{\"op\":\"lut_upload\",\"ok\":true}", true);
    return;
  }
  if (strcmp(op, "lut_set_frames") == 0) {
    const char* prev = doc["prev"] | "";
    const char* next = doc["next"] | "";
    if (!prev[0] || !next[0]) {
      replyErr(reqId, "bad_path", "prev/next 路径必填");
      return;
    }
    if (!M4WaveformLab::setSdFrames(prev, next)) {
      replyErr(reqId, "bad_frames", "SD 帧不存在或大小不是 48000");
      return;
    }
    replyOk(reqId, "{\"op\":\"lut_set_frames\",\"ok\":true}", true);
    return;
  }
  if (strcmp(op, "lut_settle") == 0) {
    const char* prev = doc["prev"] | "";
    const char* next = doc["next"] | "";
    if (!prev[0] || !next[0]) {
      replyErr(reqId, "bad_path", "prev/next 路径必填");
      return;
    }
    const uint32_t ms = M4WaveformLab::runSettle(prev, next);
    if (ms == 0) {
      replyErr(reqId, "not_ready", "帧或 LUT 未就绪");
      return;
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"op\":\"lut_settle\",\"ok\":true,\"ms\":%u}",
             static_cast<unsigned>(ms));
    replyOk(reqId, out);
    return;
  }
  if (strcmp(op, "lut_wipe") == 0) {
    const char* prev = doc["prev"] | "";
    const char* next = doc["next"] | "";
    const int steps = doc["steps"] | 8;
    const uint32_t tailMs = doc["tail_ms"] | 0;
    const int winMult = doc["win_mult"] | 1;  // window width in step units
    const int dir = doc["dir"] | 0;
    if (!prev[0] || !next[0]) {
      replyErr(reqId, "bad_path", "prev/next 路径必填");
      return;
    }
    const uint32_t ms = M4WaveformLab::runAnimateWindow(prev, next, steps, tailMs, winMult, dir);
    if (ms == 0) {
      replyErr(reqId, "not_ready", "帧或 LUT 未就绪");
      return;
    }
    char out[128];
    snprintf(out, sizeof(out),
             "{\"op\":\"lut_wipe\",\"ok\":true,\"ms\":%u,\"steps\":%d,\"win_mult\":%d,"
             "\"dir\":%d,\"refreshes\":%d}",
             static_cast<unsigned>(ms), steps, winMult, dir, steps);
    replyOk(reqId, out);
    return;
  }
  if (strcmp(op, "lut_animate") == 0) {
    const char* prev = doc["prev"] | "";
    const char* next = doc["next"] | "";
    const int steps = doc["steps"] | 6;
    const int feather = doc["feather"] | 0;
    const uint32_t tailMs = doc["tail_ms"] | 0;
    const int dir = doc["dir"] | 0;
    if (!prev[0] || !next[0]) {
      replyErr(reqId, "bad_path", "prev/next 路径必填");
      return;
    }
    // Synchronous full-frame synthesis animation (blocking; the loop stays
    // inside poll for the animation duration — verified stable on device).
    const uint32_t ms = M4WaveformLab::runAnimate(prev, next, steps, feather, tailMs, dir);
    if (ms == 0) {
      replyErr(reqId, "not_ready", "帧或 LUT 未就绪");
      return;
    }
    char out[128];
    snprintf(out, sizeof(out), "{\"op\":\"lut_animate\",\"ok\":true,\"ms\":%u,\"steps\":%d}",
             static_cast<unsigned>(ms), steps);
    replyOk(reqId, out);
    return;
  }
  if (strcmp(op, "lut_baseline") == 0) {
    const char* frame = doc["frame"] | "";
    if (!frame[0]) {
      replyErr(reqId, "bad_path", "frame 路径必填");
      return;
    }
    if (!M4WaveformLab::baselineFromSd(frame)) {
      replyErr(reqId, "bad_frame", "SD 帧不存在或 FULL 刷新失败");
      return;
    }
    replyOk(reqId, "{\"op\":\"lut_baseline\",\"ok\":true}");
    return;
  }
  if (strcmp(op, "lut_run") == 0) {
    const bool swap = doc["swap"] | false;
    const uint32_t ms = M4WaveformLab::runRefresh(swap);
    if (ms == 0) {
      replyErr(reqId, "not_ready", "帧或 LUT 未就绪");
      return;
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"op\":\"lut_run\",\"ok\":true,\"ms\":%u}", static_cast<unsigned>(ms));
    replyOk(reqId, out);
    return;
  }
  if (strcmp(op, "lut_swap") == 0) {
    M4WaveformLab::swapSlots();
    replyOk(reqId, "{\"op\":\"lut_swap\",\"ok\":true}");
    return;
  }
  if (strcmp(op, "lut_stats") == 0) {
    const auto s = M4WaveformLab::stats();
    const bool anim = M4WaveformLab::animateActive();
    char out[200];
    snprintf(out, sizeof(out),
             "{\"op\":\"lut_stats\",\"ok\":true,\"last_ms\":%u,\"runs\":%u,\"lut_set\":%s,"
             "\"active\":%s,\"frames_ready\":%s,\"animating\":%s}",
             static_cast<unsigned>(s.lastRunMs), static_cast<unsigned>(s.runs),
             s.lutSet ? "true" : "false", s.active ? "true" : "false",
             s.framesReady ? "true" : "false", anim ? "true" : "false");
    replyOk(reqId, out);
    return;
  }
  if (strcmp(op, "lut_clear") == 0) {
    M4WaveformLab::clearAll();
    replyOk(reqId, "{\"op\":\"lut_clear\",\"ok\":true}");
    return;
  }
  if (strcmp(op, "lut_end") == 0) {
    abortUpload(false);
    labFrameActive_ = false;
    replyOk(reqId, "{\"op\":\"lut_end\",\"ok\":true}", true);
    return;
  }

  if (strcmp(op, "install_begin") == 0) {  // Idempotent: same req id already handled via tryIdemReplay at top.
    if (uploadActive_ || shotActive_) {
      replyErr(reqId, "busy", "设备忙，请稍后重试");
      return;
    }
    const char* name = doc["name"] | "";
    const uint32_t size = doc["size"] | 0;
    const char* sha = doc["sha256"] | "";
    const char* verr = M4SerialDebugPolicy::validateInboxFilename(name);
    if (verr) {
      replyErr(reqId, verr, "文件名/路径非法");
      return;
    }
    if (size == 0 || size > kMaxPackageBytes) {
      replyErr(reqId, "size_invalid", "包大小非法或超过上限");
      return;
    }
    if (!M4SerialDebugPolicy::isHex64(sha)) {
      replyErr(reqId, "sha_invalid", "SHA-256 格式无效");
      return;
    }
    if (!beginUpload(reqId, name, size, sha)) {
      replyErr(reqId, "upload_begin", "无法创建暂存文件");
      return;
    }
    replyOk(reqId, "{\"op\":\"install_begin\",\"ready\":true}", true);
    return;
  }

  if (strcmp(op, "install_abort") == 0) {
    abortUpload(true);
    replyOk(reqId, "{\"op\":\"install_abort\",\"ok\":true}", true);
    return;
  }

  if (strcmp(op, "install_commit") == 0) {
    finishUploadAndInstall(reqId);
    return;
  }

  // Bulk transfer over Wi-Fi (device HTTP client). Serial only carries control.
  // Host serves the .m4x on LAN; see m4adb install --transport wifi|auto.
  if (strcmp(op, "install_http") == 0) {
    if (uploadActive_ || shotActive_) {
      replyErr(reqId, "busy", "设备忙，请稍后重试");
      return;
    }
    const char* name = doc["name"] | "";
    const uint32_t size = doc["size"] | 0;
    const char* sha = doc["sha256"] | "";
    const char* url = doc["url"] | "";
    const char* verr = M4SerialDebugPolicy::validateInboxFilename(name);
    if (verr) {
      replyErr(reqId, verr, "文件名/路径非法");
      return;
    }
    if (size == 0 || size > kMaxPackageBytes) {
      replyErr(reqId, "size_invalid", "包大小非法或超过上限");
      return;
    }
    if (!M4SerialDebugPolicy::isHex64(sha)) {
      replyErr(reqId, "sha_invalid", "SHA-256 格式无效");
      return;
    }
    const char* uerr = M4SerialDebugPolicy::validateInstallHttpUrl(url);
    if (uerr) {
      replyErr(reqId, uerr, "安装 URL 非法（仅允许局域网 http://IPv4）");
      return;
    }
    installFromHttpUrl(reqId, name, size, sha, url);
    return;
  }

  if (strcmp(op, "launch") == 0) {
    const char* appId = doc["app_id"] | "";
    if (!M4xIsValidPackageId(appId)) {
      replyErr(reqId, "invalid_id", "应用 ID 非法", true);
      return;
    }
    if (!hooks_.launchApp) {
      replyErr(reqId, "no_hook", "启动钩子未注册", true);
      return;
    }
    std::string ek, em;
    if (!hooks_.launchApp(appId, ek, em)) {
      replyErr(reqId, ek.c_str(), em.c_str(), true);
      return;
    }
    if (hooks_.noteActiveApp) hooks_.noteActiveApp(appId);
    char out[160];
    snprintf(out, sizeof(out), "{\"op\":\"launch\",\"app_id\":\"%s\"}", appId);
    replyOk(reqId, out, true);
    return;
  }

  if (strcmp(op, "tap") == 0) {
    if (!input_) {
      replyErr(reqId, "no_input", "输入管理器不可用");
      return;
    }
    const int x = doc["x"] | -1;
    const int y = doc["y"] | -1;
    bool busy = false;
    if (!input_->injectSyntheticTap(x, y, busy)) {
      replyErr(reqId, busy ? "busy" : "tap_oob", busy ? "输入忙，请稍后重试" : "点击坐标越界");
      return;
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"op\":\"tap\",\"x\":%d,\"y\":%d}", x, y);
    replyOk(reqId, out, true);
    return;
  }

  if (strcmp(op, "swipe") == 0) {
    if (!input_) {
      replyErr(reqId, "no_input", "输入管理器不可用");
      return;
    }
    const int sx = doc["sx"] | -1;
    const int sy = doc["sy"] | -1;
    const int ex = doc["ex"] | -1;
    const int ey = doc["ey"] | -1;
    bool busy = false;
    if (!input_->injectSyntheticSwipe(sx, sy, ex, ey, busy)) {
      replyErr(reqId, busy ? "busy" : "swipe_oob",
               busy ? "输入忙，请稍后重试" : "滑动坐标越界或轨迹为空");
      return;
    }
    char out[128];
    snprintf(out, sizeof(out), "{\"op\":\"swipe\",\"sx\":%d,\"sy\":%d,\"ex\":%d,\"ey\":%d}",
             sx, sy, ex, ey);
    replyOk(reqId, out, true);
    return;
  }

  if (strcmp(op, "key") == 0) {
    if (!input_) {
      replyErr(reqId, "no_input", "输入管理器不可用");
      return;
    }
    const char* name = doc["name"] | "";
    MappedInputManager::Button btn;
    if (!parseKeyName(name, btn)) {
      replyErr(reqId, "bad_key", "不支持的按键名");
      return;
    }
    bool busy = false;
    if (!input_->injectSyntheticKey(btn, busy)) {
      replyErr(reqId, busy ? "busy" : "key_fail", busy ? "输入忙，请稍后重试" : "按键注入失败");
      return;
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"op\":\"key\",\"name\":\"%s\"}", name);
    replyOk(reqId, out, true);
    return;
  }

  if (strcmp(op, "back") == 0) {
    if (!input_) {
      replyErr(reqId, "no_input", "输入管理器不可用");
      return;
    }
    bool busy = false;
    if (!input_->injectSyntheticKey(MappedInputManager::Button::Back, busy)) {
      replyErr(reqId, busy ? "busy" : "key_fail", busy ? "输入忙，请稍后重试" : "返回键注入失败");
      return;
    }
    replyOk(reqId, "{\"op\":\"back\"}", true);
    return;
  }

  if (strcmp(op, "home") == 0) {
    if (hooks_.goHome) hooks_.goHome();
    if (hooks_.clearActiveApp) hooks_.clearActiveApp();
    replyOk(reqId, "{\"op\":\"home\"}", true);
    return;
  }

  if (strcmp(op, "screenshot") == 0) {
    if (shotActive_ || uploadActive_) {
      replyErr(reqId, "busy", "截图进行中或设备忙");
      return;
    }
    if (!renderer_ || !renderer_->getFrameBuffer()) {
      replyErr(reqId, "no_fb", "帧缓冲不可用");
      return;
    }
    beginScreenshot(reqId);
    return;
  }

  replyErr(reqId, "unknown_op", "未知操作");
}

bool Bridge::beginUpload(const char* /*reqId*/, const char* name, uint32_t size, const char* shaHex) {
  abortUpload(true);
  M4xInstaller::ensureLayout();
  char part[140];
  snprintf(part, sizeof(part), "%s/%s.part", M4xPaths::kInbox, name);
  FsFile* f = new FsFile();
  if (!f || !SdMan.openFileForWrite("M4Dbg", part, *f)) {
    delete f;
    return false;
  }
  uploadFile_ = f;
  uploadActive_ = true;
  strncpy(uploadName_, name, sizeof(uploadName_) - 1);
  uploadName_[sizeof(uploadName_) - 1] = 0;
  strncpy(uploadShaHex_, shaHex, 64);
  uploadShaHex_[64] = 0;
  chunk_.begin(size);
  lastChunkAckJson_[0] = 0;
  auto* ctx = reinterpret_cast<mbedtls_sha256_context*>(shaCtx_);
  static_assert(sizeof(shaCtx_) >= sizeof(mbedtls_sha256_context), "sha ctx");
  mbedtls_sha256_init(ctx);
  mbedtls_sha256_starts(ctx, 0);
  shaReady_ = true;
  return true;
}

void Bridge::abortUpload(bool removePart) {
  if (uploadFile_) {
    auto* f = static_cast<FsFile*>(uploadFile_);
    f->close();
    delete f;
    uploadFile_ = nullptr;
  }
  if (shaReady_) {
    mbedtls_sha256_free(reinterpret_cast<mbedtls_sha256_context*>(shaCtx_));
    shaReady_ = false;
  }
  if (removePart && uploadName_[0]) {
    char part[140];
    snprintf(part, sizeof(part), "%s/%s.part", M4xPaths::kInbox, uploadName_);
    if (SdMan.exists(part)) SdMan.remove(part);
  }
  uploadActive_ = false;
  uploadName_[0] = 0;
  chunk_.reset();
  lastChunkAckJson_[0] = 0;
  labFrameActive_ = false;
}

void Bridge::handleChk(const char* reqId, uint32_t seq, uint32_t total, const uint8_t* data, size_t len) {
  const char* err = nullptr;
  const auto acc = chunk_.evaluate(reqId, seq, total, len, &err);
  if (acc == M4SerialDebugPolicy::ChunkAccept::ReplayLast) {
    if (lastChunkAckJson_[0]) {
      replyOk(reqId, lastChunkAckJson_, true);
    } else {
      replyErr(reqId, "seq_mismatch", "分片序号不连续");
    }
    return;
  }
  if (acc == M4SerialDebugPolicy::ChunkAccept::Reject) {
    replyErr(reqId, err ? err : "bad_chunk", "分片被拒绝");
    if (err && (strcmp(err, "seq_mismatch") == 0 || strcmp(err, "total_mismatch") == 0 ||
                strcmp(err, "size_overflow") == 0)) {
      abortUpload(true);
    }
    return;
  }

  if (labFrameActive_) {
    if (!M4WaveformLab::frameChunkAppend(data, len)) {
      replyErr(reqId, "lab_frame", "帧上传失败");
      abortUpload(false);
      return;
    }
    chunk_.commitAccept(reqId, seq, total, len);
    if (chunk_.bytesReceived == chunk_.byteTotal) {
      M4WaveformLab::endFrameUpload(labFrameSlot_);
      labFrameActive_ = false;
    }
    snprintf(lastChunkAckJson_, sizeof(lastChunkAckJson_), "{\"chunk\":%u,\"received\":%u}",
             static_cast<unsigned>(seq), static_cast<unsigned>(chunk_.bytesReceived));
    replyOk(reqId, lastChunkAckJson_, true);
    return;
  }

  auto* f = static_cast<FsFile*>(uploadFile_);
  if (!f || !writeFileAll(*f, data, len)) {
    replyErr(reqId, "sd_write", "SD 写入失败");
    abortUpload(true);
    return;
  }
  if (shaReady_) {
    mbedtls_sha256_update(reinterpret_cast<mbedtls_sha256_context*>(shaCtx_), data, len);
  }
  chunk_.commitAccept(reqId, seq, total, len);
  snprintf(lastChunkAckJson_, sizeof(lastChunkAckJson_), "{\"chunk\":%u,\"received\":%u}",
           static_cast<unsigned>(seq), static_cast<unsigned>(chunk_.bytesReceived));
  replyOk(reqId, lastChunkAckJson_, true);
}

bool Bridge::hashFileSha256Hex(const char* path, char outHex[65]) {
  if (!path || !outHex) return false;

  // A freshly closed file can briefly expose a stale FAT/cache cursor on
  // ESP32 SdFat, especially when the upload crossed a cluster boundary.  A
  // complete file hash is cheap compared with package extraction, so retry
  // the *whole* read from offset zero instead of failing after four zero-byte
  // reads at one boundary.  This also avoids hashing a partially advanced
  // cursor after a transient read error.
  constexpr unsigned kPasses = 3;
  constexpr unsigned kNoProgressLimit = 24;
  for (unsigned pass = 0; pass < kPasses; ++pass) {
    FsFile f;
    if (!SdMan.openFileForRead("M4Dbg", path, f)) return false;
    const size_t expected = static_cast<size_t>(f.fileSize());
    if (!f.seek(0)) {
      f.close();
      if (pass + 1 < kPasses) {
        delay(8);
        continue;
      }
      return false;
    }
    size_t remaining = expected;
    unsigned noProgress = 0;
    bool failed = false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    uint8_t buf[1024];
    while (remaining > 0) {
      const size_t want = std::min(remaining, sizeof(buf));
      const int n = f.read(buf, want);
      if (n == 0) {
        if (++noProgress >= kNoProgressLimit) {
          failed = true;
          break;
        }
        delay(4);
        continue;
      }
      noProgress = 0;
      if (n < 0 || static_cast<size_t>(n) > remaining) {
        failed = true;
        break;
      }
      mbedtls_sha256_update(&ctx, buf, static_cast<size_t>(n));
      remaining -= static_cast<size_t>(n);
    }
    uint8_t dig[32] = {};
    if (!failed && remaining == 0) mbedtls_sha256_finish(&ctx, dig);
    mbedtls_sha256_free(&ctx);
    f.close();
    if (!failed && remaining == 0) {
      for (int i = 0; i < 32; ++i) sprintf(outHex + i * 2, "%02x", dig[i]);
      outHex[64] = 0;
      return true;
    }
    if (pass + 1 < kPasses) delay(12);
  }
  return false;
}

void Bridge::installStagedInboxPackage(const char* reqId, const char* name, const char* shaHex,
                                       const char* finalPath, const char* partPath) {
  char bakPath[140];
  snprintf(bakPath, sizeof(bakPath), "%s/%s.bak", M4xPaths::kInbox, name);

  // Content no-op: same SHA as existing final inbox package of same name.
  if (SdMan.exists(finalPath)) {
    char existingHex[65] = {};
    if (hashFileSha256Hex(finalPath, existingHex) && M4SerialDebugPolicy::isContentNoop(shaHex, existingHex)) {
      if (partPath && partPath[0] && SdMan.exists(partPath)) SdMan.remove(partPath);
      auto probe = M4xInstaller::probe(finalPath);
      char out[280];
      if (probe.ok) {
        snprintf(out, sizeof(out),
                 "{\"op\":\"install\",\"noop\":true,\"id\":\"%s\",\"version\":\"%s\",\"versionCode\":%d,"
                 "\"transport\":\"staged\"}",
                 probe.manifest.id.c_str(), probe.manifest.version.c_str(), probe.manifest.versionCode);
      } else {
        snprintf(out, sizeof(out),
                 "{\"op\":\"install\",\"noop\":true,\"id\":\"\",\"version\":\"\",\"versionCode\":0,"
                 "\"transport\":\"staged\"}");
      }
      replyOk(reqId, out, true);
      return;
    }
  }

  if (SdMan.exists(bakPath)) SdMan.remove(bakPath);
  const bool hadFinal = SdMan.exists(finalPath);
  if (hadFinal) {
    if (!SdMan.rename(finalPath, bakPath)) {
      replyErr(reqId, "rename_fail", "无法隔离旧安装包", true);
      return;
    }
  }
  if (!SdMan.rename(partPath, finalPath)) {
    if (hadFinal && SdMan.exists(bakPath)) {
      SdMan.rename(bakPath, finalPath);
    }
    replyErr(reqId, "rename_fail", "原子重命名失败", true);
    return;
  }
  if (SdMan.exists(bakPath)) SdMan.remove(bakPath);

  if (!hooks_.installSync) {
    replyErr(reqId, "no_hook", "安装钩子未注册", true);
    return;
  }
  std::string ek, em, id, ver;
  int code = 0;
  const bool ok = hooks_.installSync(finalPath, ek, em, id, ver, code);
  if (!ok) {
    replyErr(reqId, ek.empty() ? "install_fail" : ek.c_str(), em.empty() ? "安装失败" : em.c_str(), true);
    return;
  }
  char out[320];
  snprintf(out, sizeof(out),
           "{\"op\":\"install\",\"noop\":false,\"id\":\"%s\",\"version\":\"%s\",\"versionCode\":%d,"
           "\"transport\":\"staged\"}",
           id.c_str(), ver.c_str(), code);
  replyOk(reqId, out, true);
}

void Bridge::finishUploadAndInstall(const char* reqId) {
  const char* err = nullptr;
  if (!chunk_.readyToCommit(&err)) {
    replyErr(reqId, err ? err : "incomplete_chunks", "上传未完成或不完整");
    abortUpload(true);
    return;
  }

  uint8_t digest[32];
  if (shaReady_) {
    auto* ctx = reinterpret_cast<mbedtls_sha256_context*>(shaCtx_);
    mbedtls_sha256_finish(ctx, digest);
    mbedtls_sha256_free(ctx);
    shaReady_ = false;
  } else {
    memset(digest, 0, sizeof(digest));
  }
  if (!hexEqSha256(uploadShaHex_, digest)) {
    replyErr(reqId, "sha_mismatch", "SHA-256 校验失败", true);
    abortUpload(true);
    return;
  }

  // Copy identity before any cleanup; then verify what actually reached the
  // card, not only the bytes that passed through the USB task.
  char nameBuf[80];
  std::strncpy(nameBuf, uploadName_, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  char shaBuf[65];
  std::strncpy(shaBuf, uploadShaHex_, 64);
  shaBuf[64] = 0;
  char part[140];
  char finalPath[128];
  snprintf(part, sizeof(part), "%s/%s.part", M4xPaths::kInbox, nameBuf);
  snprintf(finalPath, sizeof(finalPath), "%s/%s", M4xPaths::kInbox, nameBuf);

  auto* f = static_cast<FsFile*>(uploadFile_);
  const bool syncOk = f && f->sync();
  if (f) {
    f->close();
    delete f;
    uploadFile_ = nullptr;
  }

  if (!syncOk) {
    replyErr(reqId, "sd_sync", "SD 卡未确认写入完成", true);
    abortUpload(true);
    return;
  }
  char persistedHex[65] = {};
  if (!hashFileSha256Hex(part, persistedHex) || !M4SerialDebugPolicy::isContentNoop(shaBuf, persistedHex)) {
    replyErr(reqId, "sd_verify_failed", "SD 卡内容校验失败", true);
    abortUpload(true);
    return;
  }

  uploadActive_ = false;
  uploadName_[0] = 0;
  chunk_.reset();

  installStagedInboxPackage(reqId, nameBuf, shaBuf, finalPath, part);
}

namespace {

struct BridgeWifiRadio final : M4xWifiConnect::IRadio {
  bool isConnected() const override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) return true;
#endif
    return WiFi.status() == WL_CONNECTED;
  }
  std::string connectedSsid() const override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) return "qemu-openeth";
#endif
    if (WiFi.status() != WL_CONNECTED) return {};
    return std::string(WiFi.SSID().c_str());
  }
  M4xWifiConnect::RadioStatus status() const override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) return M4xWifiConnect::RadioStatus::Connected;
#endif
    const wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) return M4xWifiConnect::RadioStatus::Connected;
    if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL || s == WL_CONNECTION_LOST)
      return M4xWifiConnect::RadioStatus::Failed;
    if (s == WL_IDLE_STATUS || s == WL_DISCONNECTED) return M4xWifiConnect::RadioStatus::Idle;
    return M4xWifiConnect::RadioStatus::Connecting;
  }
  void begin(const std::string& ssid, const std::string& password) override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) {
      Serial.printf("[M4DBG] QEMU open_eth already up (ssid=%s ignored)\n", ssid.c_str());
      return;
    }
#endif
    Serial.printf("[M4DBG] WiFi begin ssid=%s\n", ssid.c_str());
    if (password.empty())
      WiFi.begin(ssid.c_str());
    else
      WiFi.begin(ssid.c_str(), password.c_str());
  }
  void disconnectKeepCreds() override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) return;
#endif
    WiFi.disconnect(false);
  }
  void setStaMode() override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) return;
#endif
    WiFi.mode(WIFI_STA);
  }
};

bool hasUsableStaAddress() {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (m4QemuNetIsUp()) return true;
#endif
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool waitForStaAddress(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (!hasUsableStaAddress() && static_cast<int32_t>(millis() - deadline) < 0) {
    delay(50);
  }
  return hasUsableStaAddress();
}

bool ensureStaConnected(uint32_t timeoutMs) {
  // A file transfer is latency-sensitive.  Keep the radio awake to avoid the
  // first HTTP request disappearing into modem sleep.
  WiFi.setSleep(false);
  if (hasUsableStaAddress()) {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    char ip[16] = {};
    m4QemuNetLocalIp(ip, sizeof(ip));
    Serial.printf("[M4DBG] WiFi/eth already up ssid=%s ip=%s\n", m4QemuNetSsid(), ip);
#else
    Serial.printf("[M4DBG] WiFi already up ssid=%s\n", WiFi.SSID().c_str());
#endif
    return true;
  }
  WIFI_STORE.loadFromFile();
  const auto& storeCreds = WIFI_STORE.getCredentials();
  if (storeCreds.empty()) {
    Serial.printf("[M4DBG] WiFi no saved credentials\n");
    return false;
  }
  std::vector<M4xWifiConnect::Credential> creds;
  creds.reserve(storeCreds.size());
  for (const auto& c : storeCreds) {
    M4xWifiConnect::Credential cr;
    cr.ssid = c.ssid;
    cr.password = c.password;
    creds.push_back(std::move(cr));
  }
  BridgeWifiRadio radio;
  M4xWifiConnect::Hooks hooks;
  hooks.nowMs = []() -> uint32_t { return millis(); };
  hooks.sleepMs = [](uint32_t ms) { delay(ms); };
  Serial.printf("[M4DBG] WiFi connectSaved timeout_ms=%u\n", static_cast<unsigned>(timeoutMs));
  Serial.flush();
  const auto r = M4xWifiConnect::connectSaved(radio, creds, static_cast<int>(timeoutMs), hooks);
  Serial.printf("[M4DBG] WiFi connectSaved ok=%d err=%s\n", r.ok ? 1 : 0, r.error.c_str());
  Serial.flush();
  if (!r.ok) return false;
  // WL_CONNECTED can precede DHCP by a short interval; do not advertise an
  // unusable 0.0.0.0 endpoint to the host.
  return waitForStaAddress(3000);
}

// Bounded LAN HTTP GET for install_http only (device is client; no bridge listener).
// Hard wall-clock so owner loop always returns an @M4DBG reply.
enum class LanDl : uint8_t { Ok, Http, File, Timeout, Size };

using LanPrgFn = std::function<void(const char* phase, int pct, uint32_t got, uint32_t total)>;

LanDl downloadLanHttpToFile(const char* url, const char* destPath, uint32_t expectSize, uint32_t wallMs,
                            const LanPrgFn& prg) {
  if (!url || !destPath || expectSize == 0) return LanDl::Http;
  const uint32_t t0 = millis();
  auto timedOut = [&]() -> bool { return static_cast<int32_t>(millis() - t0) >= static_cast<int32_t>(wallMs); };
  auto emit = [&](const char* phase, int pct, uint32_t got) {
    if (prg) prg(phase, pct, got, expectSize);
  };

  emit("http_begin", 0, 0);
  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  // Connect + read ceilings (ms). Keep below wallMs.
  const int slice = static_cast<int>(wallMs > 5000 ? wallMs - 1000 : wallMs);
  http.setConnectTimeout(slice > 3000 ? 3000 : slice);
  http.setTimeout(slice > 5000 ? 5000 : slice);
  if (!http.begin(client, url)) {
    Serial.printf("[M4DBG] http.begin failed\n");
    return LanDl::Http;
  }
  Serial.printf("[M4DBG] HTTP GET %s\n", url);
  Serial.flush();
  emit("http_get", 0, 0);
  const int code = http.GET();
  if (timedOut()) {
    http.end();
    return LanDl::Timeout;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[M4DBG] HTTP GET code=%d\n", code);
    http.end();
    return LanDl::Http;
  }
  const int cl = http.getSize();  // -1 if unknown
  if (cl > 0 && static_cast<uint32_t>(cl) != expectSize) {
    Serial.printf("[M4DBG] Content-Length %d != expect %u\n", cl, static_cast<unsigned>(expectSize));
    http.end();
    return LanDl::Size;
  }
  if (SdMan.exists(destPath)) SdMan.remove(destPath);
  FsFile file;
  if (!SdMan.openFileForWrite("M4DBG", destPath, file)) {
    http.end();
    return LanDl::File;
  }
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    file.close();
    SdMan.remove(destPath);
    http.end();
    return LanDl::Http;
  }
  emit("download", 0, 0);
  uint8_t buf[1024];
  uint32_t got = 0;
  int lastPct = -1;
  uint32_t lastPrgMs = t0;
  while (got < expectSize) {
    if (timedOut()) {
      file.close();
      SdMan.remove(destPath);
      http.end();
      return LanDl::Timeout;
    }
    const int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected() && avail <= 0) break;
      delay(1);
      // Heartbeat every 2s so host never sits in a silent timeout.
      if (static_cast<int32_t>(millis() - lastPrgMs) >= 2000) {
        const int pct = expectSize ? static_cast<int>((got * 100u) / expectSize) : 0;
        emit("download", pct, got);
        lastPrgMs = millis();
      }
      continue;
    }
    size_t want = static_cast<size_t>(avail);
    if (want > sizeof(buf)) want = sizeof(buf);
    if (want > expectSize - got) want = expectSize - got;
    const int n = stream->readBytes(reinterpret_cast<char*>(buf), want);
    if (n <= 0) {
      delay(1);
      continue;
    }
    if (!writeFileAll(file, buf, static_cast<size_t>(n))) {
      file.close();
      SdMan.remove(destPath);
      http.end();
      return LanDl::File;
    }
    got += static_cast<uint32_t>(n);
    const int pct = expectSize ? static_cast<int>((got * 100u) / expectSize) : 0;
    // Every 10% or 2s — host sees continuous progress instead of a dead wait.
    if (pct >= lastPct + 10 || static_cast<int32_t>(millis() - lastPrgMs) >= 2000 || got == expectSize) {
      emit("download", pct > 100 ? 100 : pct, got);
      lastPct = pct;
      lastPrgMs = millis();
    }
    if ((got & 0x0fff) == 0) {
      delay(0);
    }
  }
  if (!file.sync() || !file.close()) {
    file.close();
    http.end();
    SdMan.remove(destPath);
    return LanDl::File;
  }
  http.end();
  if (got != expectSize) {
    Serial.printf("[M4DBG] download got=%u expect=%u\n", static_cast<unsigned>(got),
                  static_cast<unsigned>(expectSize));
    SdMan.remove(destPath);
    return LanDl::Size;
  }
  emit("download", 100, got);
  Serial.printf("[M4DBG] download ok bytes=%u ms=%u\n", static_cast<unsigned>(got),
                static_cast<unsigned>(millis() - t0));
  Serial.flush();
  return LanDl::Ok;
}

}  // namespace

void Bridge::installFromHttpUrl(const char* reqId, const char* name, uint32_t size, const char* shaHex,
                                const char* url) {
  noteHostActivity();
  Serial.printf("[M4DBG] install_http name=%s size=%u url=%s\n", name, static_cast<unsigned>(size),
                url ? url : "");
  Serial.flush();

  auto emitPrg = [this, reqId](const char* phase, int pct, uint32_t got, uint32_t total) {
    char json[192];
    snprintf(json, sizeof(json),
             "{\"op\":\"install_http\",\"phase\":\"%s\",\"pct\":%d,\"bytes\":%u,\"total\":%u}",
             phase ? phase : "?", pct, static_cast<unsigned>(got), static_cast<unsigned>(total));
    replyProgress(reqId, json);
  };

  {
    char json[160];
    snprintf(json, sizeof(json),
             "{\"op\":\"install_http\",\"phase\":\"accepted\",\"pct\":0,\"bytes\":0,\"total\":%u}",
             static_cast<unsigned>(size));
    replyProgress(reqId, json);
  }

  // Prefer already-up STA; only then try saved credentials (bounded).
  replyProgress(reqId, "{\"op\":\"install_http\",\"phase\":\"wifi\",\"pct\":0,\"bytes\":0,\"total\":0}");
  if (!ensureStaConnected(30000)) {
    replyErr(reqId, "wifi_required", "需要 Wi-Fi：请先用 USB wifi_prepare 或在设备保存并连接同一局域网", true);
    return;
  }
  replyProgress(reqId, "{\"op\":\"install_http\",\"phase\":\"wifi_ok\",\"pct\":5,\"bytes\":0,\"total\":0}");

  char part[140];
  char finalPath[128];
  snprintf(part, sizeof(part), "%s/%s.part", M4xPaths::kInbox, name);
  snprintf(finalPath, sizeof(finalPath), "%s/%s", M4xPaths::kInbox, name);

  // Fast path: existing inbox file already matches content hash.
  if (SdMan.exists(finalPath)) {
    char existingHex[65] = {};
    if (hashFileSha256Hex(finalPath, existingHex) && M4SerialDebugPolicy::isContentNoop(shaHex, existingHex)) {
      auto probe = M4xInstaller::probe(finalPath);
      char out[320];
      if (probe.ok) {
        snprintf(out, sizeof(out),
                 "{\"op\":\"install\",\"noop\":true,\"id\":\"%s\",\"version\":\"%s\",\"versionCode\":%d,"
                 "\"transport\":\"wifi\"}",
                 probe.manifest.id.c_str(), probe.manifest.version.c_str(), probe.manifest.versionCode);
      } else {
        snprintf(out, sizeof(out),
                 "{\"op\":\"install\",\"noop\":true,\"id\":\"\",\"version\":\"\",\"versionCode\":0,"
                 "\"transport\":\"wifi\"}");
      }
      replyOk(reqId, out, true);
      return;
    }
  }

  if (SdMan.exists(part)) SdMan.remove(part);
  SdMan.mkdir(M4xPaths::kInbox, true);

  // Wall: connect+download for a small .m4x should be well under 30s on LAN.
  const LanDl dl = downloadLanHttpToFile(url, part, size, 30000, emitPrg);
  if (dl != LanDl::Ok) {
    if (SdMan.exists(part)) SdMan.remove(part);
    if (dl == LanDl::Timeout) {
      replyErr(reqId, "http_timeout", "Wi-Fi 下载超时（检查同网与主机防火墙）", true);
    } else if (dl == LanDl::Size) {
      replyErr(reqId, "size_mismatch", "下载大小与声明不一致", true);
    } else if (dl == LanDl::File) {
      replyErr(reqId, "sd_write", "SD 写入失败", true);
    } else {
      replyErr(reqId, "http_download", "Wi-Fi 下载失败", true);
    }
    return;
  }

  replyProgress(reqId, "{\"op\":\"install_http\",\"phase\":\"verify\",\"pct\":90,\"bytes\":0,\"total\":0}");
  char digHex[65] = {};
  if (!hashFileSha256Hex(part, digHex) || !M4SerialDebugPolicy::isContentNoop(shaHex, digHex)) {
    SdMan.remove(part);
    replyErr(reqId, "sha_mismatch", "SHA-256 校验失败", true);
    return;
  }

  char bakPath[140];
  snprintf(bakPath, sizeof(bakPath), "%s/%s.bak", M4xPaths::kInbox, name);
  if (SdMan.exists(bakPath)) SdMan.remove(bakPath);
  const bool hadFinal = SdMan.exists(finalPath);
  if (hadFinal) {
    if (!SdMan.rename(finalPath, bakPath)) {
      SdMan.remove(part);
      replyErr(reqId, "rename_fail", "无法隔离旧安装包", true);
      return;
    }
  }
  if (!SdMan.rename(part, finalPath)) {
    if (hadFinal && SdMan.exists(bakPath)) SdMan.rename(bakPath, finalPath);
    replyErr(reqId, "rename_fail", "原子重命名失败", true);
    return;
  }
  if (SdMan.exists(bakPath)) SdMan.remove(bakPath);

  if (!hooks_.installSync) {
    replyErr(reqId, "no_hook", "安装钩子未注册", true);
    return;
  }
  replyProgress(reqId, "{\"op\":\"install_http\",\"phase\":\"install\",\"pct\":95,\"bytes\":0,\"total\":0}");
  std::string ek, em, id, ver;
  int code = 0;
  const bool ok = hooks_.installSync(finalPath, ek, em, id, ver, code);
  if (!ok) {
    replyErr(reqId, ek.empty() ? "install_fail" : ek.c_str(), em.empty() ? "安装失败" : em.c_str(), true);
    return;
  }
  char out[320];
  snprintf(out, sizeof(out),
           "{\"op\":\"install\",\"noop\":false,\"id\":\"%s\",\"version\":\"%s\",\"versionCode\":%d,"
           "\"transport\":\"wifi\"}",
           id.c_str(), ver.c_str(), code);
  replyOk(reqId, out, true);
  Serial.printf("[M4DBG] install_http done id=%s ver=%s\n", id.c_str(), ver.c_str());
  Serial.flush();
}

bool Bridge::logicalPixelBlack(int x, int y) const {
  if (!renderer_) return false;
  const uint8_t* fb = renderer_->getFrameBuffer();
  if (!fb) return false;
  int phyX = 0, phyY = 0;
  rotateToPhysical(renderer_->getOrientation(), x, y, &phyX, &phyY);
  if (phyX < 0 || phyX >= HalDisplay::DISPLAY_WIDTH || phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) {
    return false;
  }
  const uint16_t byteIndex = phyY * HalDisplay::DISPLAY_WIDTH_BYTES + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);
  return (fb[byteIndex] & (1 << bitPosition)) == 0;
}

void Bridge::beginScreenshot(const char* reqId) {
  if (uploadActive_ || shaReady_) {
    replyErr(reqId, "busy", "设备忙，无法截图");
    return;
  }
  shotActive_ = true;
  strncpy(shotReqId_, reqId, kMaxReqIdLen);
  shotReqId_[kMaxReqIdLen] = 0;
  shotOffset_ = 0;
  const int w = renderer_->getScreenWidth();
  const int h = renderer_->getScreenHeight();
  shotTotal_ = static_cast<uint32_t>(M4SerialDebugPolicy::pbmTotalBytes(w, h));
  char meta[256];
  snprintf(meta, sizeof(meta),
           "{\"op\":\"screenshot\",\"w\":%d,\"h\":%d,\"orientation\":%d,\"bytes\":%u,\"format\":\"pbm_raw\"}", w, h,
           static_cast<int>(renderer_->getOrientation()), static_cast<unsigned>(shotTotal_));
  replyOk(reqId, meta);
  auto* sctx = reinterpret_cast<mbedtls_sha256_context*>(shaCtx_);
  mbedtls_sha256_init(sctx);
  mbedtls_sha256_starts(sctx, 0);
  shaReady_ = true;
}

void Bridge::pollScreenshot() {
  if (!shotActive_ || !renderer_) return;
  const int w = renderer_->getScreenWidth();
  const int h = renderer_->getScreenHeight();
  const uint32_t rowBytes = static_cast<uint32_t>(M4SerialDebugPolicy::pbmRowBytes(w));
  if (shotOffset_ >= shotTotal_) {
    uint8_t digest[32];
    if (shaReady_) {
      auto* sctx = reinterpret_cast<mbedtls_sha256_context*>(shaCtx_);
      mbedtls_sha256_finish(sctx, digest);
      mbedtls_sha256_free(sctx);
      shaReady_ = false;
    } else {
      memset(digest, 0, 32);
    }
    char hex[65];
    for (int i = 0; i < 32; ++i) sprintf(hex + i * 2, "%02x", digest[i]);
    char fin[160];
    snprintf(fin, sizeof(fin),
             "{\"op\":\"screenshot_done\",\"sha256\":\"%s\",\"bytes\":%u,\"chunks\":%u}", hex,
             static_cast<unsigned>(shotTotal_),
             static_cast<unsigned>((shotTotal_ + kScreenshotRawChunk - 1) / kScreenshotRawChunk));
    replyOk(shotReqId_, fin);
    shotActive_ = false;
    return;
  }

  uint8_t raw[kScreenshotRawChunk];
  size_t produced = 0;
  while (produced < kScreenshotRawChunk && shotOffset_ < shotTotal_) {
    const uint32_t row = shotOffset_ / rowBytes;
    const uint32_t colByte = shotOffset_ % rowBytes;
    if (row >= static_cast<uint32_t>(h)) break;
    bool bits[8] = {};
    int valid = 0;
    for (int bit = 0; bit < 8; ++bit) {
      const int x = static_cast<int>(colByte * 8 + bit);
      if (x >= w) break;
      bits[bit] = logicalPixelBlack(x, static_cast<int>(row));
      ++valid;
    }
    raw[produced++] = M4SerialDebugPolicy::packPbmByte(bits, valid);
    shotOffset_++;
  }
  if (shaReady_ && produced > 0) {
    mbedtls_sha256_update(reinterpret_cast<mbedtls_sha256_context*>(shaCtx_), raw, produced);
  }
  char b64[700];
  M4SerialDebugPolicy::b64Encode(raw, produced, b64, sizeof(b64));
  const uint32_t seq = (shotOffset_ - static_cast<uint32_t>(produced)) / kScreenshotRawChunk;
  Serial.printf("%s %s chk %u %u %s\n", kPrefix, shotReqId_, static_cast<unsigned>(seq),
                static_cast<unsigned>((shotTotal_ + kScreenshotRawChunk - 1) / kScreenshotRawChunk), b64);
  Serial.flush();
}

bool Bridge::hexEqSha256(const char* hex64, const uint8_t digest[32]) {
  if (!hex64 || strlen(hex64) != 64) return false;
  for (int i = 0; i < 32; ++i) {
    char pair[3] = {hex64[i * 2], hex64[i * 2 + 1], 0};
    char* end = nullptr;
    const unsigned long v = strtoul(pair, &end, 16);
    if (!end || *end) return false;
    if (static_cast<uint8_t>(v) != digest[i]) return false;
  }
  return true;
}

}  // namespace M4SerialDebug

#endif  // CROSSPOINT_MURPHY_M4

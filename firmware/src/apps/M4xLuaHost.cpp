// Must be first: force Lua integer path before any header pulls luaconf.h.
#define LUA_32BITS 1
#define LUA_C89_NUMBERS 1

#include "apps/M4xLuaHost.h"
#include "apps/M4xFsRange.h"
#include "apps/M4xHttpBodyReader.h"
#include "apps/M4xHostIo.h"
#include "apps/M4xJsonStream.h"
#include "apps/M4xProgressiveLoader.h"
#include "apps/M4UiStyleAdapter.h"
#include "apps/M4xLuaSandbox.h"
#include "apps/M4ContentProviderCatalog.h"
#include "apps/M4xNetPolicy.h"
#include "apps/M4xPathSafe.h"
#include "apps/M4xPaths.h"
#include "apps/M4PluginReaderSession.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/M4xPsvtsExtract.h"
#include "apps/M4xStateFile.h"
#include "util/M4PluginReaderBridge.h"
#include "util/M4PluginReaderStatePolicy.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4xUiListPolicy.h"
#include "util/M4xJsonScan.h"
#include "apps/M4xWifiConnect.h"
#include "apps/weread/WereadCrypto.h"
#include "CrossPointSettings.h"
#include "fontIds.h"
#include "util/M4FontPolicy.h"
#include "util/M4UiText.h"
#include "util/M4xAppFontMap.h"
#include "util/QRCodeHelper.h"
#include "util/UrlUtils.h"
#include "WifiCredentialStore.h"
#include "qemu/M4QemuNet.h"

#include <EpdFontLoader.h>
#include <mbedtls/sha256.h>

// Full types for the reused keep-alive connection members (netTls_/netHttp_).
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

// Lua must see clean macros; Arduino min/max break luaconf under C++.
#include "lua_arduino_compat.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <Arduino.h>
#include <cstdint>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <time.h>

#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

M4xLuaHost* gHost = nullptr;

// Resolve plugin list scenes through the active system theme.  Subtitles use
// the native two-line row rhythm; keeping the adjustment in one place avoids
// the old drift where the renderer used 72 px rows but touch/pagination still
// assumed 52 px rows.
M4UiStyle::Theme uiSceneStyleFor(const M4xLuaHost& host, bool hasSubtitles) {
  const int w = host.renderer_ ? host.renderer_->getScreenWidth() : 480;
  const int h = host.renderer_ ? host.renderer_->getScreenHeight() : 800;
  M4UiStyle::Theme style = M4UiStyleAdapter::current(w, h);
  if (hasSubtitles) {
    style.list.rowHeight = style.list.subtitleRowHeight;
    style.list.visibleRows = style.metrics.content.height > 0
                                 ? std::max(1, style.metrics.content.height / style.list.rowHeight)
                                 : 1;
  }
  return style;
}

M4UiStyle::Theme uiSceneStyle(const M4xLuaHost& host) {
  return uiSceneStyleFor(host, host.uiScene().hasSubtitles);
}

// ESP-IDF default Mozilla CA bundle (linked with mbedtls cert bundle).
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

size_t netMaxBodyBytes() {
#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM)
  // Prefer PSRAM-backed large responses when SPIRAM is present.
  if (psramFound()) return M4xNetPolicy::kMaxBodyWithPsram;
#endif
  return M4xNetPolicy::kMaxBodyInternalRam;
}

struct NetBodyBuf {
  char* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
  bool fromCaps = false;

  ~NetBodyBuf() { clear(); }
  void clear() {
    if (data) {
      if (fromCaps) heap_caps_free(data);
      else free(data);
      data = nullptr;
    }
    len = cap = 0;
    fromCaps = false;
  }
  bool reserve(size_t n) {
    if (n <= cap) return true;
    char* p = static_cast<char*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool caps = true;
    if (!p) {
      p = static_cast<char*>(malloc(n));
      caps = false;
    }
    if (!p) return false;
    if (data && len) std::memcpy(p, data, len);
    const size_t oldLen = len;
    if (data) {
      if (fromCaps) heap_caps_free(data);
      else free(data);
    }
    data = p;
    cap = n;
    len = oldLen;
    fromCaps = caps;
    return true;
  }
  bool append(const char* src, size_t n, size_t maxTotal) {
    if (len + n > maxTotal) return false;
    if (len + n > cap) {
      size_t want = cap ? cap * 2 : 4096;
      while (want < len + n) want *= 2;
      if (want > maxTotal) want = maxTotal;
      if (!reserve(want)) return false;
    }
    std::memcpy(data + len, src, n);
    len += n;
    return true;
  }
};

void configureTlsClient(WiFiClientSecure* secure, const M4xLuaHost* host) {
  if (!secure) return;
  // Credential-free content sources (fanqie is login-less) skip certificate
  // verification: its chapter mirror (fq-book.nat.netsite.cc:8043) uses a
  // ZeroSSL chain absent from the device bundle, so verified handshakes fail.
  // Credential-bearing apps (WeRead sends cookies) keep full CA verification.
  if (host && host->app_.id == "com.fanqie.client") {
    secure->setInsecure();
    secure->setHandshakeTimeout(30);
    return;
  }
  // Enable embedded CA bundle verification (not setInsecure).
  const size_t bundleSize = static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start);
  if (bundleSize > 0) {
    secure->setCACertBundle(x509_crt_bundle_start, bundleSize);
  }
  secure->setHandshakeTimeout(30);
}

// Arduino HTTPClient::addHeader appends — a default UA + plugin UA yields two
// User-Agent headers.  Some CDNs (jjwxc app-cdn) drop the connection on dual UA.
// Only inject the host default when the caller did not supply one.
static bool headerListHasUA(const std::vector<std::pair<std::string, std::string>>& headers) {
  for (const auto& hv : headers) {
    if (hv.first.empty()) continue;
    const std::string low = M4xNetPolicy::toLowerAscii(hv.first);
    if (low == "user-agent") return true;
  }
  return false;
}
static void applyHttpHeaders(HTTPClient& http,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const char* defaultUa) {
  if (defaultUa && defaultUa[0] && !headerListHasUA(headers)) {
    http.addHeader("User-Agent", defaultUa);
  }
  for (const auto& hv : headers) {
    if (!hv.first.empty()) http.addHeader(hv.first.c_str(), hv.second.c_str());
  }
}

struct WifiHttpStream : M4xHttp::Stream {
  NetworkClient* client = nullptr;
  explicit WifiHttpStream(NetworkClient* c) : client(c) {}
  int available() override { return client ? client->available() : 0; }
  int read(uint8_t* buf, size_t n) override {
    if (!client || !n) return 0;
    return client->read(buf, n);
  }
  bool connected() override { return client && client->connected(); }
};

// Production grow: preserve Buffer::len (decode offset). growCtx = NetBodyBuf*.
static bool netBodyBufGrow(M4xHttp::Buffer* self, size_t need) {
  auto* nb = static_cast<NetBodyBuf*>(self->growCtx);
  if (!nb || !self) return false;
  const size_t keepLen = self->len;  // never reset from nb->len (often 0)
  nb->len = keepLen;
  if (!nb->reserve(need)) return false;
  self->data = nb->data;
  self->cap = nb->cap;
  self->len = keepLen;
  return true;
}
static void httpWaitDelay() { delay(1); }
static uint32_t httpNowMs() { return static_cast<uint32_t>(millis()); }
static bool httpIsCancelled() {
  return gHost && gHost->isCancelRequested();
}
static bool headerIsChunked(const std::vector<M4xNetPolicy::ResponseHeader>& headers) {
  for (const auto& h : headers) {
    if (M4xNetPolicy::toLowerAscii(h.name) == "transfer-encoding") {
      if (M4xNetPolicy::toLowerAscii(h.value).find("chunked") != std::string::npos) return true;
    }
  }
  return false;
}

// Cap body for Lua string push: leave ≥25% of remaining Lua heap free.
static size_t maxBodyForLuaHost(size_t requested) {
  if (!gHost) return requested;
  const size_t headroom = gHost->luaMemHeadroom();
  const size_t usable = (headroom * 3) / 4;
  if (usable < 256) return 0;
  return std::min(requested, usable);
}

// Push net result table: ok, status, body, error, headers, set_cookie
void pushNetResult(lua_State* L, bool ok, int status, const char* body, size_t bodyLen, const char* err,
                   const std::vector<M4xNetPolicy::ResponseHeader>& respHeaders) {
  std::string errStr = err ? err : "";
  size_t pushLen = bodyLen;
  bool bodyOk = ok;
  if (body && bodyLen > 0) {
    const size_t cap = maxBodyForLuaHost(bodyLen);
    if (cap < bodyLen) {
      bodyOk = false;
      pushLen = 0;
      if (errStr.empty()) errStr = "response_too_large_for_lua";
    }
  }
  lua_newtable(L);
  lua_pushboolean(L, bodyOk ? 1 : 0);
  lua_setfield(L, -2, "ok");
  lua_pushnumber(L, status);
  lua_setfield(L, -2, "status");
  lua_pushlstring(L, (body && pushLen) ? body : "", pushLen);
  lua_setfield(L, -2, "body");
  lua_pushstring(L, errStr.c_str());
  lua_setfield(L, -2, "error");

  // headers: map name -> string or array of strings when multi-valued
  lua_newtable(L);
  std::map<std::string, std::vector<std::string>> grouped;
  for (const auto& h : respHeaders) {
    grouped[h.name].push_back(h.value);
  }
  for (const auto& kv : grouped) {
    if (kv.second.size() == 1) {
      lua_pushlstring(L, kv.second[0].data(), kv.second[0].size());
    } else {
      lua_newtable(L);
      int i = 1;
      for (const auto& v : kv.second) {
        lua_pushlstring(L, v.data(), v.size());
        lua_rawseti(L, -2, i++);
      }
    }
    lua_setfield(L, -2, kv.first.c_str());
  }
  lua_setfield(L, -2, "headers");

  // Convenience: set_cookie array (all Set-Cookie lines, order preserved)
  const auto cookies = M4xNetPolicy::allSetCookieValues(respHeaders);
  lua_newtable(L);
  for (size_t i = 0; i < cookies.size(); ++i) {
    lua_pushlstring(L, cookies[i].data(), cookies[i].size());
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_setfield(L, -2, "set_cookie");
}

M4xLuaHost* hostFromLua(lua_State* L) {
  (void)L;
  return gHost;
}

size_t uiSceneRowCount(const M4xLuaHost::UiListScene& scene) {
  if (!scene.fromFile) return scene.rows.size();
  return scene.fileSource ? scene.fileSource->rowCount() : 0;
}

M4FileRows::PageResult uiSceneFilePage(const M4xLuaHost::UiListScene& scene, int page) {
  if (!scene.fileSource) return {};
  return scene.fileSource->readPage(page);
}

class SdSeekableInput final : public M4FileRows::ISeekableInput {
 public:
  explicit SdSeekableInput(const char* path) {
    opened_ = path && SdMan.openFileForRead("UIF", path, file_);
  }
  ~SdSeekableInput() override {
    if (opened_) file_.close();
  }
  bool opened() const { return opened_; }
  uint64_t size() const override { return opened_ ? file_.fileSize() : 0; }
  bool seek(uint64_t offset) override { return opened_ && file_.seek(offset); }
  int64_t read(uint8_t* dst, size_t capacity) override {
    if (!opened_) return -1;
    return file_.read(dst, capacity);
  }

 private:
  FsFile file_;
  bool opened_ = false;
};

bool hasPerm(const M4xInstalledApp& app, const char* perm) {
  for (const auto& p : app.permissions) {
    if (p == perm) return true;
  }
  return false;
}

M4xHostIo::Permissions hostIoPermissions(const M4xInstalledApp& app) {
  M4xHostIo::Permissions p;
  p.network = hasPerm(app, "network");
  p.appData = hasPerm(app, "filesystem.appdata");
  return p;
}

// Resolve plugin semantic font → system content face already in GfxRenderer.
// Reuses SETTINGS.getReaderFontId() + EpdFontLoader promotion (NOTOSANS_* /
// custom hash IDs). Builtin m4_ui_cjk subset remains registered when SD
// canonical is absent — never block the plugin for a missing font file.
// Layout metrics follow original UI sizes (10→UI_10, 12→UI_12, 16→body);
// glyphs use the reader face scaled via M4UiText (chapter-list strategy).
int mapFontId(int font, GfxRenderer* renderer) {
  if (renderer) {
    EpdFontLoader::ensureFontsFromSd(*renderer);
  }
  const int readerId = SETTINGS.getReaderFontId();
  return M4xAppFontMap::mapSemanticToSystem(font, readerId);
}

// Semantic size → layout face for scale-to-match (do not hardcode large sizes).
int layoutFontForSemantic(int semantic) { return M4UiText::layoutFontForSemantic(semantic); }

struct MappedFace {
  int fontId;
  float scale;
};

MappedFace mapFace(int semantic, GfxRenderer* renderer, const char* text = nullptr) {
  MappedFace out{UI_12_FONT_ID, 1.0f};
  if (!renderer) return out;
  EpdFontLoader::ensureFontsFromSd(*renderer);
  const int layout = layoutFontForSemantic(semantic);
  // Use shared UI text path: reader face scaled to semantic UI metrics.
  const auto face = M4UiText::resolveForText(*renderer, layout, text);
  out.fontId = face.fontId;
  out.scale = face.scale;
  // Semantic 16 prefers native reader scale (body size); do not force shrink to UI_12.
  if (semantic == M4xAppFontMap::kSemanticLarge) {
    out.fontId = mapFontId(semantic, renderer);
    out.scale = 1.0f;
  }
  return out;
}

bool sandboxDataPath(const M4xLuaHost* h, const char* rel, std::string& absOut) {
  if (!h || !rel || rel[0] == '\0') return false;
  // App data paths: reuse package path rules (no abs / .. / control).
  if (!M4xPathSafe::isSafePackageRelPath(rel)) return false;
  absOut = h->dataDir_;
  if (!absOut.empty() && absOut.back() != '/') absOut += '/';
  absOut += rel;
  return true;
}

bool sandboxInstallPath(const M4xLuaHost* h, const char* rel, std::string& absOut) {
  if (!h || !rel || rel[0] == '\0') return false;
  if (!M4xPathSafe::isSafePackageRelPath(rel)) return false;
  absOut = h->installDir_;
  if (!absOut.empty() && absOut.back() != '/') absOut += '/';
  absOut += rel;
  return true;
}

uint32_t hostNowMs() { return static_cast<uint32_t>(millis()); }

struct FsFileCtx {
  FsFile* f = nullptr;
};

int fsReadChunk(void* ctx, uint8_t* dest, size_t n) {
  auto* c = static_cast<FsFileCtx*>(ctx);
  if (!c || !c->f) return -1;
  // SdFat's read path is more reliable with bounded requests; in
  // particular, a freshly-promoted 60–70 KiB Lua entry can otherwise return
  // zero on some cards even though the file is present.  readExact() still
  // loops until the requested logical length is satisfied.
  const size_t want = std::min<size_t>(n, 4096);
  return c->f->read(dest, want);
}

int fsWriteChunk(void* ctx, const uint8_t* src, size_t n) {
  auto* c = static_cast<FsFileCtx*>(ctx);
  if (!c || !c->f) return -1;
  const size_t want = std::min<size_t>(n, 4096);
  return c->f->write(src, want);
}

// ---- log / gui ----

// Developer-mode SD error log (app data: logs/error.log). Caps size so a noisy
// plugin cannot fill the card; rotates to error.log.prev once.
constexpr size_t kPluginErrorLogMaxBytes = 128 * 1024;
constexpr size_t kPluginErrorLogLineMax = 480;

bool looksLikeErrorLogLine(const char* s) {
  if (!s || !s[0]) return false;
  // Cheap ASCII fold for keyword scan (UTF-8 multi-byte left intact).
  char buf[256];
  size_t n = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p && n + 1 < sizeof(buf); ++p) {
    buf[n++] = static_cast<char>((*p < 0x80) ? tolower(*p) : *p);
  }
  buf[n] = '\0';
  static const char* kKeys[] = {"error", "fail", "oom", "panic", "exception", "fatal",
                                "assert", "traceback", "low_mem", "err=", "err:", "timeout",
                                "missing:", "denied", "bad_", "no_"};
  for (const char* k : kKeys) {
    if (std::strstr(buf, k) != nullptr) return true;
  }
  // Common CJK failure markers (no fold needed).
  if (std::strstr(s, "失败") != nullptr || std::strstr(s, "错误") != nullptr) return true;
  return false;
}

void appendPluginErrorLog(M4xLuaHost* h, const char* msg) {
  if (!h || !msg || !msg[0]) return;
  if (SETTINGS.developerSerialDebugEnabled == 0) return;
  if (!hasPerm(h->app_, "filesystem.appdata")) return;

  std::string dir = h->dataDir();
  if (dir.empty()) return;
  if (dir.back() != '/') dir += '/';
  dir += "logs";
  SdMan.mkdir(dir.c_str(), true);
  const std::string path = dir + "/error.log";
  const std::string prev = dir + "/error.log.prev";

  // Rotate when over cap (best-effort; never throw into Lua).
  if (SdMan.exists(path.c_str())) {
    FsFile probe;
    if (SdMan.openFileForRead("M4xLog", path.c_str(), probe)) {
      const size_t sz = probe.fileSize();
      probe.close();
      if (sz >= kPluginErrorLogMaxBytes) {
        if (SdMan.exists(prev.c_str())) SdMan.remove(prev.c_str());
        SdMan.rename(path.c_str(), prev.c_str());
      }
    }
  }

  FsFile f = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    // First create may race mkdir; retry once.
    SdMan.mkdir(dir.c_str(), true);
    f = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  }
  if (!f) {
    Serial.printf("[M4xLog] append fail path=%s\n", path.c_str());
    return;
  }

  char head[48];
  snprintf(head, sizeof(head), "[%lu] ", static_cast<unsigned long>(millis()));
  f.write(reinterpret_cast<const uint8_t*>(head), std::strlen(head));

  size_t len = std::strlen(msg);
  if (len > kPluginErrorLogLineMax) len = kPluginErrorLogLineMax;
  f.write(reinterpret_cast<const uint8_t*>(msg), len);
  if (len == kPluginErrorLogLineMax) {
    static const char kTrunc[] = "…";
    f.write(reinterpret_cast<const uint8_t*>(kTrunc), sizeof(kTrunc) - 1);
  }
  static const char kNl[] = "\n";
  f.write(reinterpret_cast<const uint8_t*>(kNl), 1);
  f.sync();
  f.close();
}

// log(msg [, level])
// level: "error" | "err" | "info" (default). Serial always.
// When 开发者选项 USB 串口调试 is ON: error lines also append to
// apps_data/<app>/logs/error.log (and info lines that look like failures).
int l_log(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  const char* level = "info";
  if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
    level = lua_tostring(L, 2);
    if (!level) level = "info";
  }
  Serial.printf("[M4xApp] %s\n", s ? s : "");
  auto* h = hostFromLua(L);
  if (!h || SETTINGS.developerSerialDebugEnabled == 0) return 0;
  bool asError = false;
  if (level[0] == 'e' || level[0] == 'E') {
    // error / err / ERROR
    asError = true;
  } else if (looksLikeErrorLogLine(s)) {
    asError = true;
  }
  if (asError) appendPluginErrorLog(h, s ? s : "");
  return 0;
}

// logError(msg) — always treated as error for SD logging when developer mode on.
int l_logError(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  Serial.printf("[M4xApp][ERR] %s\n", s ? s : "");
  auto* h = hostFromLua(L);
  if (h) appendPluginErrorLog(h, s ? s : "");
  return 0;
}

// sys.developerMode() -> bool  (设置→开发者选项→USB 串口调试)
int l_sys_developerMode(lua_State* L) {
  lua_pushboolean(L, SETTINGS.developerSerialDebugEnabled != 0 ? 1 : 0);
  return 1;
}

int l_gui_width(lua_State* L) {
  auto* h = hostFromLua(L);
  lua_pushnumber(L, h && h->renderer_ ? h->renderer_->getScreenWidth() : 480);
  return 1;
}

int l_gui_height(lua_State* L) {
  auto* h = hostFromLua(L);
  lua_pushnumber(L, h && h->renderer_ ? h->renderer_->getScreenHeight() : 800);
  return 1;
}

int l_gui_clear(lua_State* L) {
  auto* h = hostFromLua(L);
  if (h && h->renderer_) h->renderer_->clearScreen();
  return 0;
}

int l_gui_drawText(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->renderer_) return 0;
  const int font = static_cast<int>(luaL_checknumber(L, 1));
  const int x = static_cast<int>(luaL_checknumber(L, 2));
  const int y = static_cast<int>(luaL_checknumber(L, 3));
  const char* text = luaL_checkstring(L, 4);
  const MappedFace face = mapFace(font, h->renderer_, text);
  h->renderer_->drawText(face.fontId, x, y, text ? text : "", true, EpdFontFamily::REGULAR, face.scale);
  return 0;
}

int l_gui_textWidth(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->renderer_) {
    lua_pushnumber(L, 0);
    return 1;
  }
  const int font = static_cast<int>(luaL_checknumber(L, 1));
  const char* text = luaL_checkstring(L, 2);
  const MappedFace face = mapFace(font, h->renderer_, text);
  lua_pushnumber(L, h->renderer_->getTextWidth(face.fontId, text ? text : "", EpdFontFamily::REGULAR, face.scale));
  return 1;
}

int l_gui_lineHeight(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->renderer_) {
    lua_pushnumber(L, 24);
    return 1;
  }
  const int font = static_cast<int>(luaL_checknumber(L, 1));
  // Keep layout geometry on the compact UI face so plugin rows do not grow
  // when the reader epdfont is taller than UI chrome.
  const int layout = layoutFontForSemantic(font);
  const int height = h->renderer_->getLineHeight(layout);
  lua_pushnumber(L, height > 0 ? height : 24);
  return 1;
}

int l_gui_drawRect(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->renderer_) return 0;
  const int x = static_cast<int>(luaL_checknumber(L, 1));
  const int y = static_cast<int>(luaL_checknumber(L, 2));
  const int w = static_cast<int>(luaL_checknumber(L, 3));
  const int ht = static_cast<int>(luaL_checknumber(L, 4));
  h->renderer_->drawRect(x, y, w, ht, true);
  return 0;
}

int l_gui_fillRect(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->renderer_) return 0;
  const int x = static_cast<int>(luaL_checknumber(L, 1));
  const int y = static_cast<int>(luaL_checknumber(L, 2));
  const int w = static_cast<int>(luaL_checknumber(L, 3));
  const int ht = static_cast<int>(luaL_checknumber(L, 4));
  h->renderer_->fillRect(x, y, w, ht, true);
  return 0;
}

int l_gui_drawLine(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->renderer_) return 0;
  const int x1 = static_cast<int>(luaL_checknumber(L, 1));
  const int y1 = static_cast<int>(luaL_checknumber(L, 2));
  const int x2 = static_cast<int>(luaL_checknumber(L, 3));
  const int y2 = static_cast<int>(luaL_checknumber(L, 4));
  h->renderer_->drawLine(x1, y1, x2, y2, true);
  return 0;
}

int l_gui_drawQR(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->renderer_) return 0;
  const int x = static_cast<int>(luaL_checknumber(L, 1));
  const int y = static_cast<int>(luaL_checknumber(L, 2));
  const char* data = luaL_checkstring(L, 3);
  const int px = lua_isnoneornil(L, 4) ? QRCodeHelper::DEFAULT_PX : static_cast<int>(luaL_checknumber(L, 4));
  if (px < 1 || px > 12 || !data) return 0;
  // Draw with auto-version selection; push success flag so plugins can surface
  // "QR too large" instead of showing a broken/undecodable code.
  const bool ok = QRCodeHelper::drawQRCode(*h->renderer_, x, y, std::string(data), static_cast<uint8_t>(px));
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_gui_qrSize(lua_State* L) {
  const int px = lua_isnoneornil(L, 1) ? QRCodeHelper::DEFAULT_PX : static_cast<int>(luaL_checknumber(L, 1));
  lua_pushnumber(L, QRCodeHelper::qrSize(static_cast<uint8_t>(px < 1 ? 1 : px)));
  return 1;
}

int l_gui_refresh(lua_State* L) {
  auto* h = hostFromLua(L);
  if (h && h->renderer_) h->renderer_->displayBuffer();
  return 0;
}

// ---- sys ----

// sys.fontInfo() -> { ok, result, path, family, hint, readerFontId, available }
// Diagnostic only: plugins must not hard-block UI on ok=false. Drawing always
// goes through mapFontId → SETTINGS.getReaderFontId() / GfxRenderer (subset or
// full CJK). available is true when the system face can paint (always on M4).
int l_sys_fontInfo(lua_State* L) {
  auto* h = hostFromLua(L);
  if (h && h->renderer_) {
    EpdFontLoader::ensureFontsFromSd(*h->renderer_);
  }
  const auto lr = EpdFontLoader::lastCanonicalLoadResult();
  const bool ok = (lr == M4FontPolicy::LoadResult::Promoted);
  const int readerId = SETTINGS.getReaderFontId();
  lua_newtable(L);
  lua_pushboolean(L, ok ? 1 : 0);
  lua_setfield(L, -2, "ok");
  lua_pushstring(L, M4FontPolicy::loadResultName(lr));
  lua_setfield(L, -2, "result");
  lua_pushstring(L, M4FontPolicy::kCanonicalSdPath);
  lua_setfield(L, -2, "path");
  lua_pushstring(L, M4FontPolicy::kCanonicalFamily);
  lua_setfield(L, -2, "family");
  // ASCII-only hint safe on builtin subset.
  if (ok) {
    lua_pushstring(L, "canonical CJK font promoted");
  } else if (lr == M4FontPolicy::LoadResult::Missing) {
    lua_pushstring(L, "system UI subset; copy NotoSansCJKsc.epdfont to /fonts/ for full CJK");
  } else if (lr == M4FontPolicy::LoadResult::InvalidHeader) {
    lua_pushstring(L, "invalid /fonts/NotoSansCJKsc.epdfont; using system subset");
  } else if (lr == M4FontPolicy::LoadResult::LoadFailed) {
    lua_pushstring(L, "failed to load canonical epdfont; using system subset");
  } else {
    lua_pushstring(L, "canonical not promoted; using system subset");
  }
  lua_setfield(L, -2, "hint");
  lua_pushinteger(L, readerId);
  lua_setfield(L, -2, "readerFontId");
  // Host always registers mandatory NOTOSANS_* / UI faces → drawing is available.
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "available");
  return 1;
}

int l_sys_millis(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(millis()));
  return 1;
}

int l_sys_time(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(time(nullptr)));
  return 1;
}

int l_sys_delay(lua_State* L) {
  const int ms = static_cast<int>(luaL_checknumber(L, 1));
  if (ms > 0 && ms < 5000) delay(static_cast<uint32_t>(ms));
  return 0;
}

int l_sys_exit(lua_State* L) {
  auto* h = hostFromLua(L);
  if (h) h->requestExit();
  return 0;
}

// sys.memInfo() -> {lua_used, lua_limit, lua_headroom, heap_free, psram_free}
// Lets plugins self-protect before heavy allocations.
int l_sys_memInfo(lua_State* L) {
  auto* h = hostFromLua(L);
  lua_newtable(L);
  lua_pushnumber(L, h ? static_cast<lua_Number>(h->luaMemUsed()) : 0);
  lua_setfield(L, -2, "lua_used");
  lua_pushnumber(L, h ? static_cast<lua_Number>(h->luaMemLimit()) : 0);
  lua_setfield(L, -2, "lua_limit");
  lua_pushnumber(L, h ? static_cast<lua_Number>(h->luaMemHeadroom()) : 0);
  lua_setfield(L, -2, "lua_headroom");
  lua_pushnumber(L, static_cast<lua_Number>(ESP.getFreeHeap()));
  lua_setfield(L, -2, "heap_free");
  lua_pushnumber(L, static_cast<lua_Number>(ESP.getFreePsram()));
  lua_setfield(L, -2, "psram_free");
  return 1;
}

// Load and run another .lua file from the installed app directory (multi-file plugins).
int l_sys_load(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  const char* rel = luaL_checkstring(L, 1);
  std::string path;
  if (!sandboxInstallPath(h, rel, path)) return luaL_error(L, "bad path");
  FsFile f;
  if (!SdMan.openFileForRead("M4xLua", path.c_str(), f)) {
    return luaL_error(L, "open failed: %s", rel);
  }
  const size_t n = f.fileSize();
  if (n > M4xPathSafe::kMaxEntryBytes) {
    f.close();
    return luaL_error(L, "module too large: %s", rel);
  }
  std::string src;
  src.resize(n);
  if (n) {
    FsFileCtx ctx{&f};
    if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, reinterpret_cast<uint8_t*>(&src[0]), n)) {
      f.close();
      return luaL_error(L, "short read: %s", rel);
    }
  }
  f.close();
  // Replace path arg with chunk, then run; return all results.
  if (luaL_loadbuffer(L, src.data(), src.size(), rel) != LUA_OK) {
    return lua_error(L);
  }
  lua_remove(L, 1);  // drop path; stack = [chunk]
  if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
    return lua_error(L);
  }
  return lua_gettop(L);
}

// ---- fs ----

// Deterministic recovery of state files after interrupted replaceFile.
// Empty live/bak files are valid. live+bak+tmp means incomplete tmp→live copy
// — restore bak (shared decide()). On I/O failure retain recoverable artifacts.
static void reconcileStateFile(const std::string& path) {
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";
  M4xStateFile::Snap snap;
  snap.liveExists = SdMan.exists(path.c_str());
  snap.bakExists = SdMan.exists(bak.c_str());
  snap.tmpExists = SdMan.exists(tmp.c_str());
  const M4xStateFile::Action act = M4xStateFile::decide(snap);

  auto copyExact = [](const char* src, const char* dst) -> bool {
    FsFile in;
    if (!SdMan.openFileForRead("M4xRec", src, in)) return false;
    const size_t n = in.fileSize();
    std::string body;
    body.resize(n);
    if (n) {
      FsFileCtx ctx{&in};
      if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, reinterpret_cast<uint8_t*>(&body[0]), n)) {
        in.close();
        return false;
      }
    }
    in.close();
    if (SdMan.exists(dst)) SdMan.remove(dst);
    FsFile out;
    if (!SdMan.openFileForWrite("M4xRec", dst, out)) return false;
    if (n) {
      FsFileCtx ctx{&out};
      if (!M4xLuaSandbox::writeExact(fsWriteChunk, &ctx, reinterpret_cast<const uint8_t*>(body.data()), n)) {
        out.close();
        SdMan.remove(dst);
        return false;
      }
    }
    out.close();
    FsFile v;
    if (!SdMan.openFileForRead("M4xRec", dst, v)) return false;
    const size_t got = v.fileSize();
    v.close();
    return got == n;
  };

  if (act == M4xStateFile::Action::UseLiveDropTmp) {
    if (snap.tmpExists) (void)SdMan.remove(tmp.c_str());
    return;
  }
  if (act == M4xStateFile::Action::RestoreBak) {
    // Bak must be readable before touching (possibly partial) live.
    FsFile probe;
    if (!SdMan.openFileForRead("M4xRec", bak.c_str(), probe)) return;
    const size_t bakSize = probe.fileSize();
    probe.close();

    // Replace partial/missing live with bak; only drop bak after live size matches.
    bool liveOk = false;
    if (SdMan.exists(path.c_str())) {
      // Prefer copy-over so bak remains until verified (rename would consume bak).
      if (copyExact(bak.c_str(), path.c_str())) {
        liveOk = true;
      } else {
        return;  // retain bak + tmp + partial live
      }
    } else if (SdMan.rename(bak.c_str(), path.c_str())) {
      liveOk = SdMan.exists(path.c_str());
    } else if (copyExact(bak.c_str(), path.c_str())) {
      liveOk = true;
    } else {
      return;
    }
    if (!liveOk) return;
    // Verify size (empty bak → empty live is valid).
    FsFile lv;
    if (!SdMan.openFileForRead("M4xRec", path.c_str(), lv)) return;
    const size_t liveSize = lv.fileSize();
    lv.close();
    if (liveSize != bakSize && SdMan.exists(bak.c_str())) {
      // copyExact already matched; if bak was renamed away, bakSize from probe is fine.
    }
    if (SdMan.exists(bak.c_str()) && M4xStateFile::mayDropBakAfterRestore(true, true)) {
      (void)SdMan.remove(bak.c_str());
    }
    if (snap.tmpExists) (void)SdMan.remove(tmp.c_str());
    return;
  }
  if (act == M4xStateFile::Action::DropTmpOnly) {
    if (snap.tmpExists) (void)SdMan.remove(tmp.c_str());
  }
}

int l_fs_readFile(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "filesystem.appdata")) {
    return luaL_error(L, "permission denied: filesystem.appdata");
  }
  const char* rel = luaL_checkstring(L, 1);
  std::string path;
  if (!sandboxDataPath(h, rel, path)) return luaL_error(L, "bad path");
  reconcileStateFile(path);
  FsFile f;
  if (!SdMan.openFileForRead("M4xLua", path.c_str(), f)) {
    lua_pushnil(L);
    return 1;
  }
  const size_t n = f.fileSize();
  if (n > M4xPathSafe::kMaxFileBytes) {
    f.close();
    return luaL_error(L, "file too large");
  }
  std::string body;
  body.resize(n);
  if (n) {
    FsFileCtx ctx{&f};
    if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, reinterpret_cast<uint8_t*>(&body[0]), n)) {
      f.close();
      lua_pushnil(L);
      return 1;
    }
  }
  f.close();
  lua_pushlstring(L, body.data(), body.size());
  return 1;
}

int l_fs_writeFile(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "filesystem.appdata")) {
    return luaL_error(L, "permission denied: filesystem.appdata");
  }
  const char* rel = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);
  std::string path;
  if (!sandboxDataPath(h, rel, path)) return luaL_error(L, "bad path");
  if (len > M4xPathSafe::kMaxFileBytes) {
    lua_pushboolean(L, 0);
    return 1;
  }
  // Ensure parent dirs for nested paths like cache/foo/bar.json
  {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
      const std::string dir = path.substr(0, slash);
      SdMan.mkdir(dir.c_str(), true);
    }
  }
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("M4xLua", path.c_str(), f)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  if (len) {
    FsFileCtx ctx{&f};
    if (!M4xLuaSandbox::writeExact(fsWriteChunk, &ctx, reinterpret_cast<const uint8_t*>(data), len)) {
      f.close();
      SdMan.remove(path.c_str());
      lua_pushboolean(L, 0);
      return 1;
    }
  }
  f.close();
  lua_pushboolean(L, 1);
  return 1;
}

// Recoverable replace: write .tmp, then live→.bak, tmp→live, drop .bak.
// Never deletes live before a durable replacement exists.
int l_fs_replaceFile(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "filesystem.appdata")) {
    return luaL_error(L, "permission denied: filesystem.appdata");
  }
  const char* rel = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);
  std::string path;
  if (!sandboxDataPath(h, rel, path)) return luaL_error(L, "bad path");
  if (len > M4xPathSafe::kMaxFileBytes) {
    lua_pushboolean(L, 0);
    return 1;
  }
  const uint32_t writeStartedMs = millis();
  reconcileStateFile(path);
  {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
      SdMan.mkdir(path.substr(0, slash).c_str(), true);
    }
  }
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";
  if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("M4xLua", tmp.c_str(), f)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  if (len) {
    FsFileCtx ctx{&f};
    if (!M4xLuaSandbox::writeExact(fsWriteChunk, &ctx, reinterpret_cast<const uint8_t*>(data), len)) {
      f.close();
      SdMan.remove(tmp.c_str());
      lua_pushboolean(L, 0);
      return 1;
    }
  }
  f.close();

  // Move live → bak (keep last good state).
  if (SdMan.exists(bak.c_str())) SdMan.remove(bak.c_str());
  if (SdMan.exists(path.c_str())) {
    if (!SdMan.rename(path.c_str(), bak.c_str())) {
      // Copy live to bak then remove live only after bak ok.
      FsFile in;
      if (!SdMan.openFileForRead("M4xLua", path.c_str(), in)) {
        SdMan.remove(tmp.c_str());
        lua_pushboolean(L, 0);
        return 1;
      }
      const size_t n = in.fileSize();
      std::string old;
      old.resize(n);
      if (n) {
        FsFileCtx ctx{&in};
        if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, reinterpret_cast<uint8_t*>(&old[0]), n)) {
          in.close();
          SdMan.remove(tmp.c_str());
          lua_pushboolean(L, 0);
          return 1;
        }
      }
      in.close();
      FsFile bout;
      if (!SdMan.openFileForWrite("M4xLua", bak.c_str(), bout)) {
        SdMan.remove(tmp.c_str());
        lua_pushboolean(L, 0);
        return 1;
      }
      if (n) {
        FsFileCtx ctx{&bout};
        if (!M4xLuaSandbox::writeExact(fsWriteChunk, &ctx, reinterpret_cast<const uint8_t*>(old.data()), n)) {
          bout.close();
          SdMan.remove(bak.c_str());
          SdMan.remove(tmp.c_str());
          lua_pushboolean(L, 0);
          return 1;
        }
      }
      bout.close();
      SdMan.remove(path.c_str());
    }
  }

  // tmp → live
  if (!SdMan.rename(tmp.c_str(), path.c_str())) {
    // Copy fallback: short write leaves live(partial)+bak+tmp for reconcile.
    FsFile in;
    if (!SdMan.openFileForRead("M4xLua", tmp.c_str(), in)) {
      if (SdMan.exists(bak.c_str())) (void)SdMan.rename(bak.c_str(), path.c_str());
      lua_pushboolean(L, 0);
      return 1;
    }
    const size_t n = in.fileSize();
    std::string body;
    body.resize(n);
    if (n) {
      FsFileCtx ctx{&in};
      if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, reinterpret_cast<uint8_t*>(&body[0]), n)) {
        in.close();
        // Keep bak + tmp; do not invent a partial live.
        lua_pushboolean(L, 0);
        return 1;
      }
    }
    in.close();
    if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
    FsFile out;
    if (!SdMan.openFileForWrite("M4xLua", path.c_str(), out)) {
      // Keep bak + tmp
      lua_pushboolean(L, 0);
      return 1;
    }
    if (n) {
      FsFileCtx ctx{&out};
      if (!M4xLuaSandbox::writeExact(fsWriteChunk, &ctx, reinterpret_cast<const uint8_t*>(body.data()), n)) {
        out.close();
        // Partial live may exist — leave live+bak+tmp for reconcileStateFile (RestoreBak).
        lua_pushboolean(L, 0);
        return 1;
      }
    }
    out.close();
    // Verify full size before dropping tmp/bak.
    FsFile v;
    if (!SdMan.openFileForRead("M4xLua", path.c_str(), v)) {
      lua_pushboolean(L, 0);
      return 1;
    }
    const size_t got = v.fileSize();
    v.close();
    if (got != n) {
      // Incomplete — retain bak + tmp for recovery (live may be partial).
      lua_pushboolean(L, 0);
      return 1;
    }
    if (!SdMan.remove(tmp.c_str())) {
      // Live complete; tmp debris OK
    }
  }

  if (SdMan.exists(bak.c_str())) (void)SdMan.remove(bak.c_str());
  Serial.printf("[WRPERF] stage=sd_write ms=%lu bytes=%u ok=1 atomic=1\n",
                static_cast<unsigned long>(millis() - writeStartedMs),
                static_cast<unsigned>(len));
  lua_pushboolean(L, 1);
  return 1;
}

// fs.fileSize(rel) -> number|nil  (byte size; nil if missing / unreadable)
// reader.openText({path,title,bookId,chapterUid,progressKey,tocPath?,chapterIndex?}) -> ok [, err]
int l_reader_openText(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_args");
    return 2;
  }
  auto getStr = [&](const char* key) -> const char* {
    lua_getfield(L, 1, key);
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s ? s : "";
  };
  const char* path = getStr("path");
  const char* title = getStr("title");
  const char* bookId = getStr("bookId");
  const char* chapterUid = getStr("chapterUid");
  const char* progressKey = getStr("progressKey");
  const char* tocPath = getStr("tocPath");
  const char* providerId = getStr("providerId");
  // Optional raw-byte restore (Lua number → uint64).
  uint64_t initialByteOffset = 0;
  bool hasInitialByteOffset = false;
  {
    lua_getfield(L, 1, "initialByteOffset");
    if (lua_isnumber(L, -1)) {
      const lua_Number n = lua_tonumber(L, -1);
      if (n >= 0 && n < static_cast<lua_Number>(UINT64_MAX)) {
        initialByteOffset = static_cast<uint64_t>(n);
        hasInitialByteOffset = true;
      }
    }
    lua_pop(L, 1);
  }
  int chapterIndex = 0;
  {
    lua_getfield(L, 1, "chapterIndex");
    if (lua_isnumber(L, -1)) {
      const int v = static_cast<int>(lua_tointeger(L, -1));
      if (v >= 0) chapterIndex = v;
    }
    lua_pop(L, 1);
  }

  const auto pe = M4PluginReaderBridge::validateRelPath(path);
  if (pe != M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, M4PluginReaderBridge::errorKey(pe));
    return 2;
  }
  const auto me = M4PluginReaderBridge::validateMeta(title, bookId, chapterUid, progressKey);
  if (me != M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, M4PluginReaderBridge::errorKey(me));
    return 2;
  }
  M4PluginReaderBridge::OpenRequest req;
  const auto re = M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir_, path, req.absPath);
  if (re != M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, M4PluginReaderBridge::errorKey(re));
    return 2;
  }
  {
    FsFile f;
    if (!SdMan.openFileForRead("M4xRdr", req.absPath.c_str(), f)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "missing");
      return 2;
    }
    // Validate restore offset against file size when provided.
    if (hasInitialByteOffset) {
      const size_t fsz = f.fileSize();
      if (fsz == 0) {
        initialByteOffset = 0;
      } else if (initialByteOffset >= static_cast<uint64_t>(fsz)) {
        initialByteOffset = static_cast<uint64_t>(fsz - 1);
      }
    }
    f.close();
  }
  // Optional book TOC for system chapter list (app-data relative JSON).
  if (tocPath && tocPath[0]) {
    if (tocPath[0] == '/' || std::strstr(tocPath, "..") != nullptr ||
        !M4xPathSafe::isSafePackageRelPath(tocPath)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "bad_toc_path");
      return 2;
    }
    const std::string prefix = (h->dataDir_.empty() || h->dataDir_.back() == '/')
                                   ? h->dataDir_
                                   : (h->dataDir_ + "/");
    req.tocRelPath = tocPath;
    req.tocAbsPath = prefix + tocPath;
  }
  req.relPath = path;
  req.title = title;
  req.bookId = bookId;
  req.chapterUid = chapterUid;
  req.progressKey = progressKey;
  req.appId = h->app_.id;
  req.generation = M4PluginReaderSession::bumpGeneration();
  req.initialByteOffset = initialByteOffset;
  req.hasInitialByteOffset = hasInitialByteOffset;
  req.chapterIndex = chapterIndex;
  if (providerId && providerId[0] &&
      M4ContentProvider::idOk(providerId, M4ContentProvider::kMaxProviderIdLen)) {
    req.providerId = providerId;
  }
  if (!M4PluginReaderSession::queueOpen(req)) {
    Serial.printf("[WR05] t=%lu openText_reject queue path=%s\n", static_cast<unsigned long>(millis()), path);
    lua_pushboolean(L, 0);
    lua_pushstring(L, "queue_rejected");
    return 2;
  }
  // Compact timing marker only (no cookies/tokens/full abs paths).
  Serial.printf("[WR05] t=%lu openText_ok gen=%u off=%u path=%s\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned>(req.generation),
                hasInitialByteOffset ? static_cast<unsigned>(initialByteOffset) : 0u, path);
  lua_pushboolean(L, 1);
  return 1;
}

// reader.openToc({tocPath?,providerId?,bookId,title,currentIndex}) -> ok [, err]
// Launches system TxtReaderChapterSelection.  Legacy plugins provide a
// bounded toc.json; ContentProvider plugins may instead provide a registered
// file-backed catalog and let the owner task resolve it without Lua copying
// chapter rows.
// Lua must not paint while native_toc owns the display (same as openText).
int l_reader_openToc(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_args");
    return 2;
  }
  auto getStr = [&](const char* key) -> const char* {
    lua_getfield(L, 1, key);
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s ? s : "";
  };
  const char* tocPath = getStr("tocPath");
  const char* providerId = getStr("providerId");
  const char* bookId = getStr("bookId");
  const char* title = getStr("title");
  int currentIndex = 0;
  {
    lua_getfield(L, 1, "currentIndex");
    if (lua_isnumber(L, -1)) {
      const int v = static_cast<int>(lua_tointeger(L, -1));
      if (v >= 0) currentIndex = v;
    }
    lua_pop(L, 1);
  }
  if (std::strlen(bookId) > M4PluginReaderBridge::kMaxIdLen ||
      std::strlen(title) > M4PluginReaderBridge::kMaxTitleLen) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_meta");
    return 2;
  }
  const std::string prefix =
      (h->dataDir_.empty() || h->dataDir_.back() == '/') ? h->dataDir_ : (h->dataDir_ + "/");
  M4PluginReaderSession::TocRequest req;
  req.appDataRoot = h->dataDir_;
  req.bookId = bookId;
  req.bookTitle = title;
  req.appId = h->app_.id;
  req.providerId = providerId;
  req.currentIndex = currentIndex;
  req.generation = M4PluginReaderSession::bumpGeneration();

  if (tocPath && tocPath[0]) {
    if (tocPath[0] == '/' || std::strstr(tocPath, "..") != nullptr ||
        !M4xPathSafe::isSafePackageRelPath(tocPath)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "bad_toc_path");
      return 2;
    }
    req.tocRelPath = tocPath;
    req.tocAbsPath = prefix + tocPath;
    FsFile f;
    if (!SdMan.openFileForRead("M4xToc", req.tocAbsPath.c_str(), f)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "missing");
      return 2;
    }
    f.close();
  } else {
    // File-backed catalogs are intentionally resolved by the owner task. The
    // registry check here gives Lua a deterministic error instead of queueing
    // a request that can only fail after the screen has changed.
    if (!providerId || !providerId[0] ||
        !M4ContentProvider::idOk(providerId, M4ContentProvider::kMaxProviderIdLen)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "empty_toc_path");
      return 2;
    }
    M4ContentProvider::ChapterCatalogSpec catalog;
    if (!M4ContentProviderSession::catalogFor(providerId, bookId, currentIndex, catalog) ||
        catalog.fileRelPath.empty()) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "catalog_not_registered");
      return 2;
    }
  }
  if (!M4PluginReaderSession::queueToc(req)) {
    Serial.printf("[WR05] t=%lu openToc_reject provider=%s path=%s\n", static_cast<unsigned long>(millis()),
                  providerId ? providerId : "", tocPath ? tocPath : "");
    lua_pushboolean(L, 0);
    lua_pushstring(L, "queue_rejected");
    return 2;
  }
  Serial.printf("[WR05] t=%lu openToc_ok gen=%u provider=%s path=%s cur=%d\n",
                static_cast<unsigned long>(millis()), static_cast<unsigned>(req.generation),
                providerId ? providerId : "", tocPath ? tocPath : "<catalog>", currentIndex);
  lua_pushboolean(L, 1);
  return 1;
}

// provider.register({providerId,bookId,title,chapters=[{uid,title}]|catalog={
//   kind="file",path,count,uidField,titleField},currentIndex?})
int l_provider_register(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  auto getStr = [&](const char* key) -> std::string {
    lua_getfield(L, 1, key);
    std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s;
  };
  M4ContentProvider::BookSpec spec;
  spec.providerId = getStr("providerId");
  spec.bookId = getStr("bookId");
  spec.title = getStr("title");
  spec.appId = h->app_.id;
  lua_getfield(L, 1, "currentIndex");
  if (lua_isnumber(L, -1)) {
    const int v = static_cast<int>(lua_tointeger(L, -1));
    if (v >= 0) spec.currentIndex0 = v;
  }
  lua_pop(L, 1);
  bool hasCatalog = false;
  lua_getfield(L, 1, "catalog");
  if (!lua_isnil(L, -1) && !lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 0);
    return 1;
  }
  if (lua_istable(L, -1)) {
    hasCatalog = true;
    lua_getfield(L, -1, "kind");
    const bool fileKind = lua_isstring(L, -1) && std::strcmp(lua_tostring(L, -1), "file") == 0;
    lua_pop(L, 1);
    lua_getfield(L, -1, "path");
    if (lua_isstring(L, -1)) spec.catalog.fileRelPath = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "count");
    if (lua_isnumber(L, -1) && lua_tointeger(L, -1) >= 0) {
      spec.catalog.chapterCount = static_cast<size_t>(lua_tointeger(L, -1));
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "uidField");
    if (lua_isnumber(L, -1)) spec.catalog.uidField0 = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "titleField");
    if (lua_isnumber(L, -1)) spec.catalog.titleField0 = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (!fileKind) {
      lua_pop(L, 1);
      lua_pushboolean(L, 0);
      return 1;
    }
    spec.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;
  }
  lua_pop(L, 1);
  lua_getfield(L, 1, "chapters");
  if (lua_istable(L, -1)) {
    const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
    if (hasCatalog && n != 0) {
      lua_pop(L, 1);
      lua_pushboolean(L, 0);
      return 1;
    }
    if (!hasCatalog && n > M4ContentProvider::kMaxInlineChapters) {
      // Reject instead of silently truncating a provider's chapter identity
      // set at the old inline limit.
      lua_pop(L, 1);
      lua_pushboolean(L, 0);
      return 1;
    }
    for (size_t i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, static_cast<int>(i));
      M4ContentProvider::ChapterMeta ch;
      if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "uid");
        if (lua_isstring(L, -1)) ch.uid = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "title");
        if (lua_isstring(L, -1)) ch.title = lua_tostring(L, -1);
        lua_pop(L, 1);
      } else if (lua_isstring(L, -1)) {
        ch.uid = lua_tostring(L, -1);
      }
      lua_pop(L, 1);
      spec.chapters.push_back(std::move(ch));
    }
  }
  lua_pop(L, 1);
  lua_pushboolean(L, M4ContentProviderSession::registerBook(spec) ? 1 : 0);
  return 1;
}

// provider.setChapter({providerId,bookId,chapterUid?,index?,state,path?,pct?,error?})
int l_provider_setChapter(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  M4ContentProvider::ChapterStatus st;
  lua_getfield(L, 1, "providerId");
  if (lua_isstring(L, -1)) st.providerId = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "bookId");
  if (lua_isstring(L, -1)) st.bookId = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "chapterUid");
  if (lua_isstring(L, -1)) st.chapterUid = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "index");
  if (lua_isnumber(L, -1)) st.index0 = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "state");
  st.state = M4ContentProvider::parseState(lua_isstring(L, -1) ? lua_tostring(L, -1) : "missing");
  lua_pop(L, 1);
  lua_getfield(L, 1, "path");
  if (lua_isstring(L, -1)) st.cacheRelPath = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "pct");
  if (lua_isnumber(L, -1)) st.pct = static_cast<int>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "error");
  if (lua_isstring(L, -1)) st.error = lua_tostring(L, -1);
  lua_pop(L, 1);
  (void)h;
  lua_pushboolean(L, M4ContentProviderSession::setChapterStatus(st) ? 1 : 0);
  return 1;
}

// provider.pollWork() -> nil | {type="prefetch", providerId, bookId,
// chapterUid, index, needsResolve, catalog={kind,path,count,uidField,titleField}}
int l_provider_pollWork(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  (void)h;
  const M4ContentProvider::PrefetchWork w = M4ContentProviderSession::pollWork();
  if (!w.valid) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  lua_pushstring(L, "prefetch");
  lua_setfield(L, -2, "type");
  lua_pushstring(L, w.providerId.c_str());
  lua_setfield(L, -2, "providerId");
  lua_pushstring(L, w.bookId.c_str());
  lua_setfield(L, -2, "bookId");
  lua_pushstring(L, w.chapterUid.c_str());
  lua_setfield(L, -2, "chapterUid");
  lua_pushinteger(L, w.index0);
  lua_setfield(L, -2, "index");
  const bool needsResolve = M4ContentProvider::requiresCatalogResolve(w);
  lua_pushboolean(L, needsResolve ? 1 : 0);
  lua_setfield(L, -2, "needsResolve");
  lua_newtable(L);
  lua_pushstring(L, w.catalog.kind == M4ContentProvider::ChapterCatalogKind::FileRows ? "file" : "inline");
  lua_setfield(L, -2, "kind");
  lua_pushstring(L, w.catalog.fileRelPath.c_str());
  lua_setfield(L, -2, "path");
  lua_pushinteger(L, static_cast<lua_Integer>(w.catalog.chapterCount));
  lua_setfield(L, -2, "count");
  lua_pushinteger(L, w.catalog.uidField0);
  lua_setfield(L, -2, "uidField");
  lua_pushinteger(L, w.catalog.titleField0);
  lua_setfield(L, -2, "titleField");
  lua_setfield(L, -2, "catalog");
  return 1;
}

// provider.resolveCatalogWork(work) -> {index,chapterUid,title}|nil,error
// This is deliberately a host-owned SD lookup. It validates the work against
// the registered book/catalog and never opens a network client.
int l_provider_resolveCatalogWork(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushnil(L);
    lua_pushstring(L, "work_required");
    return 2;
  }
  if (!hasPerm(h->app_, "filesystem.appdata")) {
    lua_pushnil(L);
    lua_pushstring(L, "permission denied: filesystem.appdata");
    return 2;
  }
  auto fieldString = [&](const char* key) {
    lua_getfield(L, 1, key);
    std::string value = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return value;
  };
  const std::string providerId = fieldString("providerId");
  const std::string bookId = fieldString("bookId");
  lua_getfield(L, 1, "index");
  const int index0 = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : -1;
  lua_pop(L, 1);
  M4ContentProvider::ChapterCatalogSpec catalog;
  if (!M4ContentProviderSession::catalogFor(providerId, bookId, index0, catalog)) {
    lua_pushnil(L);
    lua_pushstring(L, "catalog_not_registered");
    return 2;
  }
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir().c_str(), catalog.fileRelPath.c_str(), abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    lua_pushnil(L);
    lua_pushstring(L, "bad_path");
    return 2;
  }

  class SdCatalogInput final : public M4FileRows::ISeekableInput {
   public:
    explicit SdCatalogInput(const char* path) { opened_ = SdMan.openFileForRead("M4CR", path, file_); }
    ~SdCatalogInput() override { file_.close(); }
    bool opened() const { return opened_; }
    uint64_t size() const override { return opened_ ? file_.fileSize() : 0; }
    bool seek(uint64_t offset) override { return opened_ && file_.seek(offset); }
    int64_t read(uint8_t* dst, size_t capacity) override {
      if (!opened_) return -1;
      const int n = file_.read(dst, capacity);
      return n < 0 ? -1 : n;
    }
   private:
    FsFile file_;
    bool opened_ = false;
  };

  auto input = std::make_unique<SdCatalogInput>(abs.c_str());
  if (!input->opened()) {
    lua_pushnil(L);
    lua_pushstring(L, "source_not_found");
    return 2;
  }
  M4FileRows::Limits limits;
  limits.maxFileBytes = 8u * 1024u * 1024u;
  limits.maxRows = M4ContentProvider::kMaxCatalogChapters;
  limits.maxLineBytes = 2048;
  limits.maxPageSize = 32;
  limits.maxCursors = 12;
  auto source = std::make_unique<M4FileRows::FileRowSource>();
  const M4FileRows::Error opened = source->open(std::move(input), 32, limits);
  if (opened != M4FileRows::Error::None) {
    lua_pushnil(L);
    lua_pushstring(L, M4FileRows::errorKey(opened));
    return 2;
  }
  M4ContentProvider::ChapterMeta resolved;
  std::string error;
  if (!M4ContentProviderCatalog::resolveRow(*source, catalog, index0, resolved, error)) {
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }
  lua_newtable(L);
  lua_pushinteger(L, index0);
  lua_setfield(L, -2, "index");
  lua_pushstring(L, resolved.uid.c_str());
  lua_setfield(L, -2, "chapterUid");
  lua_pushstring(L, resolved.title.c_str());
  lua_setfield(L, -2, "title");
  return 1;
}

// provider.takeResume() -> nil | {appId,providerId,bookId,title,chapterUid,...}
// One-shot Home/Recent m4cp reopen intent. Must include last chapter identity
// so cold start can open cached body before login/session network work.
int l_provider_takeResume(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  M4ContentProviderSession::HistoryResume r;
  if (!M4ContentProviderSession::takeHistoryResume(r)) {
    lua_pushnil(L);
    return 1;
  }
  if (!r.appId.empty() && r.appId != h->app_.id) {
    // Not for this app — re-queue for correct app.
    M4ContentProviderSession::queueHistoryResume(r);
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  lua_pushstring(L, r.appId.c_str());
  lua_setfield(L, -2, "appId");
  lua_pushstring(L, r.providerId.c_str());
  lua_setfield(L, -2, "providerId");
  lua_pushstring(L, r.bookId.c_str());
  lua_setfield(L, -2, "bookId");
  lua_pushstring(L, r.title.c_str());
  lua_setfield(L, -2, "title");
  lua_pushstring(L, r.chapterUid.c_str());
  lua_setfield(L, -2, "chapterUid");
  lua_pushstring(L, r.cacheRelPath.c_str());
  lua_setfield(L, -2, "cacheRelPath");
  lua_pushinteger(L, r.chapterIndex0);
  lua_setfield(L, -2, "chapterIndex");
  if (r.hasByteOffset) {
    lua_pushinteger(L, static_cast<lua_Integer>(r.byteOffset));
    lua_setfield(L, -2, "byteOffset");
  }
  return 1;
}

// provider.historyUri(providerId, bookId) -> string|nil
int l_provider_historyUri(lua_State* L) {
  const char* providerId = luaL_checkstring(L, 1);
  const char* bookId = luaL_checkstring(L, 2);
  const std::string u = M4ContentProvider::makeHistoryUri(providerId, bookId);
  if (u.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, u.c_str());
  }
  return 1;
}

int l_fs_fileSize(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "filesystem.appdata")) {
    return luaL_error(L, "permission denied: filesystem.appdata");
  }
  const char* rel = luaL_checkstring(L, 1);
  std::string path;
  if (!sandboxDataPath(h, rel, path)) return luaL_error(L, "bad path");
  reconcileStateFile(path);
  FsFile f;
  if (!SdMan.openFileForRead("M4xLua", path.c_str(), f)) {
    lua_pushnil(L);
    return 1;
  }
  const size_t n = f.fileSize();
  f.close();
  lua_pushnumber(L, static_cast<lua_Number>(n));
  return 1;
}

// fs.readRange(rel, offset, length) -> string | nil [, err]
// offset 0-based; length in 1..kMaxReadRangeBytes. EOF may short-read.
// Never loads the whole file into a std::string — only the requested window.
int l_fs_readRange(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "filesystem.appdata")) {
    return luaL_error(L, "permission denied: filesystem.appdata");
  }
  const char* rel = luaL_checkstring(L, 1);
  const lua_Integer offset = luaL_checkinteger(L, 2);
  const lua_Integer length = luaL_checkinteger(L, 3);
  const M4xFsRange::ArgError ae =
      M4xFsRange::validateArgs(rel, static_cast<long long>(offset), static_cast<long long>(length));
  if (ae != M4xFsRange::ArgError::Ok) {
    lua_pushnil(L);
    lua_pushstring(L, M4xFsRange::argErrorKey(ae));
    return 2;
  }
  std::string path;
  if (!sandboxDataPath(h, rel, path)) {
    lua_pushnil(L);
    lua_pushstring(L, "bad_path");
    return 2;
  }
  reconcileStateFile(path);
  FsFile f;
  if (!SdMan.openFileForRead("M4xLua", path.c_str(), f)) {
    lua_pushnil(L);
    return 1;
  }
  const size_t fsz = f.fileSize();
  const size_t want = static_cast<size_t>(length);
  const size_t toRead =
      M4xFsRange::clampReadLength(static_cast<uint64_t>(fsz), static_cast<uint64_t>(offset), want);
  if (toRead == 0) {
    f.close();
    lua_pushlstring(L, "", 0);
    return 1;
  }
  if (!f.seek(static_cast<uint64_t>(offset))) {
    f.close();
    lua_pushnil(L);
    lua_pushstring(L, "seek_failed");
    return 2;
  }
  // M4xRuntime task stack is only ~12 KiB — never put a 16 KiB window on it.
  // Prefer PSRAM; fall back to internal heap. Only-window allocation (toRead).
  static_assert(!M4xFsRange::isStackBufferSafe(M4xFsRange::kMaxLength),
                "full readRange max must not be treated as stack-safe");
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(toRead, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  bool fromCaps = true;
  if (!buf) {
    buf = static_cast<uint8_t*>(malloc(toRead));
    fromCaps = false;
  }
  if (!buf) {
    f.close();
    lua_pushnil(L);
    lua_pushstring(L, "oom");
    return 2;
  }
  FsFileCtx ctx{&f};
  if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, buf, toRead)) {
    f.close();
    if (fromCaps) heap_caps_free(buf);
    else free(buf);
    lua_pushnil(L);
    lua_pushstring(L, "short_read");
    return 2;
  }
  f.close();
  lua_pushlstring(L, reinterpret_cast<const char*>(buf), toRead);
  if (fromCaps) heap_caps_free(buf);
  else free(buf);
  return 1;
}

// fs.readCatalogRow(rel, index0) -> row | nil,error
// Reads one bounded catalog row (zero-based index). The file is scanned only
// for bounded metadata and the returned page; no catalog-sized Lua/C++ vector
// is created. This deliberately uses the same UID/title resolver as provider
// work so direct Catalog.virtual_rows access has the same validation rules.
int l_fs_readCatalogRow(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "filesystem.appdata")) {
    return luaL_error(L, "permission denied: filesystem.appdata");
  }
  const char* rel = luaL_checkstring(L, 1);
  const lua_Integer requested = luaL_checkinteger(L, 2);
  if (requested < 0 || requested > static_cast<lua_Integer>(M4ContentProvider::kMaxCatalogChapters)) {
    lua_pushnil(L);
    lua_pushstring(L, "row_out_of_range");
    return 2;
  }
  std::string abs;
  if (!sandboxDataPath(h, rel, abs)) {
    lua_pushnil(L);
    lua_pushstring(L, "bad_path");
    return 2;
  }

  class SdCatalogRowInput final : public M4FileRows::ISeekableInput {
   public:
    explicit SdCatalogRowInput(const char* path) { opened_ = SdMan.openFileForRead("M4FR", path, file_); }
    ~SdCatalogRowInput() override { file_.close(); }
    bool opened() const { return opened_; }
    uint64_t size() const override { return opened_ ? file_.fileSize() : 0; }
    bool seek(uint64_t offset) override { return opened_ && file_.seek(offset); }
    int64_t read(uint8_t* dst, size_t capacity) override {
      if (!opened_) return -1;
      const int n = file_.read(dst, capacity);
      return n < 0 ? -1 : n;
    }

   private:
    FsFile file_;
    bool opened_ = false;
  };

  auto input = std::make_unique<SdCatalogRowInput>(abs.c_str());
  if (!input->opened()) {
    lua_pushnil(L);
    lua_pushstring(L, "source_not_found");
    return 2;
  }
  M4FileRows::Limits limits;
  limits.maxFileBytes = 8u * 1024u * 1024u;
  limits.maxRows = M4ContentProvider::kMaxCatalogChapters;
  limits.maxLineBytes = 2048;
  limits.maxPageSize = 32;
  limits.maxCursors = 12;
  auto source = std::make_unique<M4FileRows::FileRowSource>();
  const M4FileRows::Error opened = source->open(std::move(input), 32, limits);
  if (opened != M4FileRows::Error::None) {
    lua_pushnil(L);
    lua_pushstring(L, M4FileRows::errorKey(opened));
    return 2;
  }
  const int index0 = static_cast<int>(requested);
  if (static_cast<size_t>(index0) >= source->rowCount()) {
    lua_pushnil(L);
    lua_pushstring(L, "row_out_of_range");
    return 2;
  }
  M4ContentProvider::ChapterCatalogSpec catalog;
  catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;
  catalog.chapterCount = source->rowCount();
  catalog.fileRelPath = rel;
  catalog.uidField0 = 0;
  catalog.titleField0 = 1;
  M4ContentProvider::ChapterMeta resolved;
  std::string error;
  std::string line;
  if (!M4ContentProviderCatalog::resolveRow(*source, catalog, index0, resolved, error, &line)) {
    lua_pushnil(L);
    lua_pushstring(L, error.c_str());
    return 2;
  }
  lua_newtable(L);
  lua_pushinteger(L, index0);
  lua_setfield(L, -2, "index");
  lua_pushstring(L, resolved.uid.c_str());
  lua_setfield(L, -2, "chapterUid");
  lua_pushstring(L, resolved.uid.c_str());
  lua_setfield(L, -2, "uid");
  lua_pushstring(L, resolved.title.c_str());
  lua_setfield(L, -2, "title");
  lua_pushstring(L, line.c_str());
  lua_setfield(L, -2, "line");
  lua_newtable(L);
  size_t start = 0;
  int field = 1;
  for (size_t i = 0; i <= line.size() && field <= 16; ++i) {
    if (i == line.size() || line[i] == '\t') {
      lua_pushlstring(L, line.data() + start, i - start);
      lua_rawseti(L, -2, field++);
      start = i + 1;
    }
  }
  lua_setfield(L, -2, "fields");
  return 1;
}

int l_fs_readAppFile(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  const char* rel = luaL_checkstring(L, 1);
  std::string path;
  if (!sandboxInstallPath(h, rel, path)) return luaL_error(L, "bad path");
  FsFile f;
  if (!SdMan.openFileForRead("M4xLua", path.c_str(), f)) {
    lua_pushnil(L);
    return 1;
  }
  const size_t n = f.fileSize();
  if (n > M4xPathSafe::kMaxFileBytes) {
    f.close();
    return luaL_error(L, "file too large");
  }
  std::string body;
  body.resize(n);
  if (n) {
    FsFileCtx ctx{&f};
    if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, reinterpret_cast<uint8_t*>(&body[0]), n)) {
      f.close();
      lua_pushnil(L);
      return 1;
    }
  }
  f.close();
  lua_pushlstring(L, body.data(), body.size());
  return 1;
}

// ---- net ----

// Device radio adapter for M4xWifiConnect (never logs passwords).
struct EspWifiRadio final : M4xWifiConnect::IRadio {
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
      Serial.printf("[M4xNet] QEMU open_eth already up (ssid=%s ignored)\n", ssid.c_str());
      return;
    }
#endif
    // Log SSID only — never password.
    Serial.printf("[M4xNet] WiFi begin ssid=%s\n", ssid.c_str());
    if (password.empty())
      WiFi.begin(ssid.c_str());
    else
      WiFi.begin(ssid.c_str(), password.c_str());
  }
  void disconnectKeepCreds() override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) return;
#endif
    // false = do not erase credentials
    WiFi.disconnect(false);
  }
  void setStaMode() override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) return;
#endif
    WiFi.mode(WIFI_STA);
  }
};

void pushConnectResult(lua_State* L, const M4xWifiConnect::ConnectResult& r) {
  lua_newtable(L);
  lua_pushboolean(L, r.ok ? 1 : 0);
  lua_setfield(L, -2, "ok");
  lua_pushstring(L, r.error.c_str());
  lua_setfield(L, -2, "error");
  if (!r.ssid.empty()) {
    lua_pushlstring(L, r.ssid.data(), r.ssid.size());
    lua_setfield(L, -2, "ssid");
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "ssid");
  }
}

int l_net_isConnected(lua_State* L) {
  // Current state only — never pretends connectSaved will succeed.
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (m4QemuNetWifiCompatConnected()) {
    lua_pushboolean(L, 1);
    return 1;
  }
#endif
  lua_pushboolean(L, WiFi.status() == WL_CONNECTED ? 1 : 0);
  return 1;
}

// net.connectSaved([timeout_ms]) -> { ok, error, ssid? }
// Loads WifiCredentialStore and tries networks in order. Claims ownership only
// when a new association is established by this host.
int l_net_connectSaved(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "network")) {
    return luaL_error(L, "permission denied: network");
  }
  int timeoutMs = M4xWifiConnect::kDefaultTimeoutMs;
  if (lua_isnumber(L, 1)) {
    timeoutMs = M4xWifiConnect::clampTimeoutMs(static_cast<int>(lua_tonumber(L, 1)));
  }
  // Keep Lua callback wall above connect budget + margin.
  h->extendCallbackWallMs(static_cast<uint32_t>(timeoutMs));

  if (h->isCancelRequested()) {
    M4xWifiConnect::ConnectResult r;
    r.error = M4xWifiConnect::kErrCancelled;
    pushConnectResult(L, r);
    return 1;
  }

  WIFI_STORE.loadFromFile();
  const auto& storeCreds = WIFI_STORE.getCredentials();
  std::vector<M4xWifiConnect::Credential> creds;
  creds.reserve(storeCreds.size());
  for (const auto& c : storeCreds) {
    M4xWifiConnect::Credential cr;
    cr.ssid = c.ssid;
    cr.password = c.password;
    creds.push_back(std::move(cr));
  }

  EspWifiRadio radio;
  M4xWifiConnect::Hooks hooks;
  hooks.nowMs = []() -> uint32_t { return millis(); };
  hooks.sleepMs = [](uint32_t ms) {
    // Yield so cooperative cancel and other tasks can run.
    delay(static_cast<uint32_t>(ms));
  };
  hooks.isCancelled = [h]() -> bool { return h->isCancelRequested(); };

  const uint32_t connectStartedMs = millis();
  const M4xWifiConnect::ConnectResult r =
      M4xWifiConnect::connectSaved(radio, creds, timeoutMs, hooks);
  Serial.printf("[WRPERF] stage=wifi_connect ms=%lu budget_ms=%d ok=%d owned=%d\n",
                static_cast<unsigned long>(millis() - connectStartedMs), timeoutMs,
                r.ok ? 1 : 0, r.owned ? 1 : 0);
  if (r.owned) {
    h->wifiOwned_ = true;
    Serial.printf("[M4xNet] connectSaved ok owned ssid=%s\n", r.ssid.c_str());
  } else if (r.ok) {
    Serial.printf("[M4xNet] connectSaved ok pre-existing ssid=%s (not owned)\n", r.ssid.c_str());
  } else {
    Serial.printf("[M4xNet] connectSaved fail error=%s\n", r.error.c_str());
  }
  pushConnectResult(L, r);
  return 1;
}

// net.request(method, url [, opts_table])
// opts: { headers = {k=v}, body = string, timeout_ms = n }
// returns: { ok, status, body, error, headers, set_cookie }
// Policy: HTTPS-only, CA bundle verify, no cross-host Cookie forward,
//         bounded body (PSRAM-aware), wall deadline on read.
int l_net_request(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "network")) {
    return luaL_error(L, "permission denied: network");
  }
  const char* method = luaL_checkstring(L, 1);
  const char* urlIn = luaL_checkstring(L, 2);
  if (!method || !urlIn) return luaL_error(L, "method/url required");

  std::vector<std::pair<std::string, std::string>> headerList;
  std::string body;
  int timeoutMs = M4xNetPolicy::kDefaultTimeoutMs;

  if (lua_istable(L, 3)) {
    lua_getfield(L, 3, "timeout_ms");
    if (lua_isnumber(L, -1)) {
      timeoutMs = M4xNetPolicy::clampTimeoutMs(static_cast<int>(lua_tonumber(L, -1)));
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "body");
    if (lua_isstring(L, -1)) {
      size_t blen = 0;
      const char* b = lua_tolstring(L, -1, &blen);
      if (b && blen) body.assign(b, blen);
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "headers");
    if (lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2) != 0) {
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
          headerList.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  auto fail = [&](const char* err, int status = -1) {
    std::vector<M4xNetPolicy::ResponseHeader> empty;
    pushNetResult(L, false, status, "", 0, err, empty);
    return 1;
  };

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (!m4QemuNetWifiCompatConnected() && WiFi.status() != WL_CONNECTED)
    return fail("wifi_not_connected");
#else
  if (WiFi.status() != WL_CONNECTED) return fail("wifi_not_connected");
#endif

  // Reclaim transient Lua objects before mbedTLS asks for large contiguous
  // internal blocks. If headroom is already unsafe, return a stable OOM code
  // instead of collapsing it into HTTPClient's generic "connection refused".
  lua_gc(L, LUA_GCCOLLECT, 0);
  const size_t largestInternal =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largestInternal < 32 * 1024) {
    Serial.printf("[M4xNet] TLS skipped: internal largest=%u free=%u psram=%u lua=%u/%u\n",
                  static_cast<unsigned>(largestInternal),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                  h ? static_cast<unsigned>(h->luaMemUsed()) : 0u,
                  h ? static_cast<unsigned>(h->luaMemLimit()) : 0u);
    return fail("oom");
  }

  std::string url = urlIn;
  std::string methodStr = method;
  for (char& c : methodStr) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (methodStr != "GET" && methodStr != "POST") return fail("unsupported_method");

  if (!M4xNetPolicy::isAllowedUrl(url)) return fail("https_required");

  const size_t maxBody = netMaxBodyBytes();
  const uint32_t deadline = millis() + static_cast<uint32_t>(timeoutMs);
  const std::string initialUrl = url;  // same-origin check for insecure mirror retry
  NetBodyBuf respBody;
  std::vector<M4xNetPolicy::ResponseHeader> respHeaders;
  std::vector<M4xNetPolicy::ResponseHeader> accumulatedSetCookies;  // same-origin hops only
  int code = -1;
  std::string err;

  auto hasHeaderCI = [&](const char* name) {
    for (const auto& hv : headerList) {
      if (M4xNetPolicy::toLowerAscii(hv.first) == M4xNetPolicy::toLowerAscii(name)) return true;
    }
    return false;
  };

  for (int hop = 0; hop <= M4xNetPolicy::kMaxRedirects; ++hop) {
    if (static_cast<int32_t>(deadline - millis()) <= 0) {
      err = "timeout";
      break;
    }
    if (!M4xNetPolicy::isAllowedUrl(url)) {
      err = "https_required";
      break;
    }

    // Content mirrors redirect to hosts whose CA chain is valid but absent
    // from the device bundle (e.g. fanqie fq-book.nat.netsite.cc:8043 uses
    // ZeroSSL ECC DV SSL CA 2).  A same-origin redirect hop that fails the
    // verified handshake is retried once without certificate verification —
    // the origin is already trusted, only the mirror's chain is unknown.
    const bool allowInsecureRetry =
        hop > 0 && !initialUrl.empty() && M4xHttp::sameOrigin(initialUrl, url);
    bool insecureRetried = false;

  retry_tls:
    std::unique_ptr<WiFiClient> client;
    {
      auto* secure = new WiFiClientSecure();
      if (insecureRetried) secure->setInsecure();
      configureTlsClient(secure, h);
      client.reset(secure);
    }

    HTTPClient http;
    http.setTimeout(timeoutMs);
    // Manual redirects so we can strip Cookie on cross-host hops.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.collectAllHeaders(true);

    if (!http.begin(*client, url.c_str())) {
      if (allowInsecureRetry && !insecureRetried) {
        insecureRetried = true;
        goto retry_tls;
      }
      err = "http_begin_failed";
      break;
    }

    if (!hasHeaderCI("Referer")) {
      http.addHeader("Referer", "https://weread.qq.com/");
    }
    applyHttpHeaders(http, headerList,
                     "Mozilla/5.0 (Linux; Android 12) AppleWebKit/537.36 M4xWeread/1.0");

    // Do not log Cookie / Authorization values.
    Serial.printf("[M4xNet] %s %s body=%u hop=%d\n", methodStr.c_str(), url.c_str(),
                  static_cast<unsigned>(body.size()), hop);

    // HTTPClient performs the verified TLS handshake and sends the request in
    // this bounded call. Pre-connecting separately would change reuse and
    // streaming semantics, so report the security/request stage together.
    const uint32_t requestStartedMs = millis();
    if (methodStr == "GET") {
      code = http.GET();
    } else {
      if (!hasHeaderCI("Content-Type")) {
        http.addHeader("Content-Type", "application/json");
      }
      code = http.POST(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(body.data())), body.size());
    }
    if (code < 0 && allowInsecureRetry && !insecureRetried) {
      http.end();
      insecureRetried = true;
      goto retry_tls;
    }
    Serial.printf("[WRPERF] stage=tls_http_request ms=%lu budget_ms=%d status=%d hop=%d\n",
                  static_cast<unsigned long>(millis() - requestStartedMs), timeoutMs, code, hop);

    respHeaders.clear();
    if (code > 0) {
      const int hc = http.headers();
      for (int i = 0; i < hc; ++i) {
        M4xNetPolicy::ResponseHeader rh;
        rh.name = http.headerName(i).c_str();
        rh.value = http.header(i).c_str();
        respHeaders.push_back(std::move(rh));
      }
    }

    // Redirect?
    if (code >= 300 && code < 400) {
      const String loc = http.getLocation();
      // Capture Set-Cookie from this hop before end() when next hop is same-origin.
      const std::string nextPreview =
          loc.length() ? M4xNetPolicy::resolveRedirectUrl(url, loc.c_str()) : std::string();
      const bool nextSame = !nextPreview.empty() && M4xHttp::sameOrigin(url, nextPreview);
      std::vector<std::string> hopCookies;
      if (nextSame) {
        for (const auto& h : respHeaders) {
          if (M4xNetPolicy::toLowerAscii(h.name) == "set-cookie") {
            accumulatedSetCookies.push_back(h);
            hopCookies.push_back(h.value);
          }
        }
      }
      http.end();
      if (loc.length() == 0) {
        err = "redirect_no_location";
        break;
      }
      const std::string next = nextPreview;
      if (next.empty() || !M4xNetPolicy::isAllowedUrl(next)) {
        err = "redirect_blocked";
        break;
      }
      // Full origin (scheme+host+port) before forwarding credentials.
      const bool same = nextSame;
      headerList = M4xNetPolicy::headersForRedirect(headerList, same);
      // Forward cookie name=value from this hop into the next request (same-origin only).
      if (same && !hopCookies.empty()) {
        M4xNetPolicy::mergeSetCookiesIntoRequestHeaders(headerList, hopCookies);
      }
      // RFC: POST→GET on 301/302/303
      if (code == 303 || code == 302 || code == 301) {
        if (methodStr == "POST") {
          methodStr = "GET";
          body.clear();
        }
      }
      url = next;
      if (hop == M4xNetPolicy::kMaxRedirects) {
        err = "too_many_redirects";
        break;
      }
      continue;
    }

    // Production M4xHttp body reader (Content-Length / chunked / until-close).
    if (code > 0) {
      NetworkClient* stream = http.getStreamPtr();
      if (stream) {
        const int contentLen = http.getSize();  // -1 if unknown
        M4xHttp::BodyKind kind;
        size_t clen = 0;
        if (headerIsChunked(respHeaders)) {
          kind = M4xHttp::BodyKind::Chunked;
        } else if (contentLen >= 0) {
          kind = M4xHttp::BodyKind::ContentLength;
          clen = static_cast<size_t>(contentLen);
        } else {
          kind = M4xHttp::BodyKind::UntilClose;
        }

        if (kind == M4xHttp::BodyKind::ContentLength && clen > maxBody) {
          err = "response_too_large";
          http.end();
          break;
        }

        M4xHttp::Limits lim;
        lim.maxBody = maxBody;
        lim.totalTimeoutMs = static_cast<uint32_t>(timeoutMs);
        lim.idleTimeoutMs = 5000;
        lim.nowMs = &httpNowMs;
        lim.onWait = &httpWaitDelay;
        lim.isCancelled = &httpIsCancelled;

        if (kind == M4xHttp::BodyKind::ContentLength) {
          if (!respBody.reserve(clen > 0 ? clen : 1)) {
            err = "oom";
            http.end();
            break;
          }
        } else {
          respBody.reserve(4096);
        }

        M4xHttp::Buffer buf;
        buf.data = respBody.data;
        buf.len = respBody.len;
        buf.cap = respBody.cap;
        buf.growCtx = &respBody;
        buf.grow = &netBodyBufGrow;

        WifiHttpStream wstream(stream);
        const uint32_t bodyStartedMs = millis();
        const M4xHttp::Result br = M4xHttp::readBody(wstream, kind, clen, buf, lim);
        Serial.printf("[WRPERF] stage=http_body_stream ms=%lu budget_ms=%d bytes=%u ok=%d\n",
                      static_cast<unsigned long>(millis() - bodyStartedMs), timeoutMs,
                      static_cast<unsigned>(buf.len), br.ok ? 1 : 0);
        respBody.len = buf.len;
        if (buf.data) {
          respBody.data = buf.data;
          respBody.cap = buf.cap;
        }
        if (!br.ok) err = br.error.empty() ? "body_read_failed" : br.error;
      }
    } else {
      err = http.errorToString(code).c_str();
    }
    http.end();
    // Merge same-origin redirect Set-Cookie into final response headers.
    for (const auto& sc : accumulatedSetCookies) {
      respHeaders.push_back(sc);
    }
    break;  // final response (or error)
  }

  const bool ok = (code >= 200 && code < 300) && err.empty();
  pushNetResult(L, ok, code, respBody.data ? respBody.data : "", respBody.len, err.c_str(), respHeaders);
  return 1;
}

// Push net.extractPsvts result: ok, status, value, error, headers, set_cookie (no body).
void pushNetPsvtsResult(lua_State* L, bool ok, int status, const std::string& value, const char* err,
                        const std::vector<M4xNetPolicy::ResponseHeader>& respHeaders) {
  lua_newtable(L);
  lua_pushboolean(L, ok ? 1 : 0);
  lua_setfield(L, -2, "ok");
  lua_pushnumber(L, status);
  lua_setfield(L, -2, "status");
  lua_pushlstring(L, value.data(), value.size());
  lua_setfield(L, -2, "value");
  lua_pushstring(L, err ? err : "");
  lua_setfield(L, -2, "error");

  lua_newtable(L);
  std::map<std::string, std::vector<std::string>> grouped;
  for (const auto& h : respHeaders) {
    grouped[h.name].push_back(h.value);
  }
  for (const auto& kv : grouped) {
    if (kv.second.size() == 1) {
      lua_pushlstring(L, kv.second[0].data(), kv.second[0].size());
    } else {
      lua_newtable(L);
      int i = 1;
      for (const auto& v : kv.second) {
        lua_pushlstring(L, v.data(), v.size());
        lua_rawseti(L, -2, i++);
      }
    }
    lua_setfield(L, -2, kv.first.c_str());
  }
  lua_setfield(L, -2, "headers");

  const auto cookies = M4xNetPolicy::allSetCookieValues(respHeaders);
  lua_newtable(L);
  for (size_t i = 0; i < cookies.size(); ++i) {
    lua_pushlstring(L, cookies[i].data(), cookies[i].size());
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_setfield(L, -2, "set_cookie");
}

// net.extractPsvts(url [, opts_table])
// Stream-scan HTTPS body for "psvts":"..." without buffering the page into Lua.
// opts: { headers = {k=v}, timeout_ms = n }  (GET only)
// returns: { ok, status, value, error, headers, set_cookie }
int l_net_extractPsvts(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !hasPerm(h->app_, "network")) {
    return luaL_error(L, "permission denied: network");
  }
  const char* urlIn = luaL_checkstring(L, 1);
  if (!urlIn) return luaL_error(L, "url required");

  std::vector<std::pair<std::string, std::string>> headerList;
  int timeoutMs = M4xNetPolicy::kDefaultTimeoutMs;

  // Accept either extractPsvts(url, opts) or extractPsvts(url) with opts at arg 2.
  const int optsIdx = 2;
  if (lua_istable(L, optsIdx)) {
    lua_getfield(L, optsIdx, "timeout_ms");
    if (lua_isnumber(L, -1)) {
      timeoutMs = M4xNetPolicy::clampTimeoutMs(static_cast<int>(lua_tonumber(L, -1)));
    }
    lua_pop(L, 1);

    lua_getfield(L, optsIdx, "headers");
    if (lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2) != 0) {
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
          headerList.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  auto fail = [&](const char* err, int status = -1) {
    std::vector<M4xNetPolicy::ResponseHeader> empty;
    pushNetPsvtsResult(L, false, status, std::string(), err, empty);
    return 1;
  };

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (!m4QemuNetWifiCompatConnected() && WiFi.status() != WL_CONNECTED)
    return fail("wifi_not_connected");
#else
  if (WiFi.status() != WL_CONNECTED) return fail("wifi_not_connected");
#endif

  lua_gc(L, LUA_GCCOLLECT, 0);
  const size_t largestInternal =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largestInternal < 32 * 1024) {
    Serial.printf("[M4xNet] TLS skipped (extractPsvts): internal largest=%u free=%u psram=%u lua=%u/%u\n",
                  static_cast<unsigned>(largestInternal),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                  h ? static_cast<unsigned>(h->luaMemUsed()) : 0u,
                  h ? static_cast<unsigned>(h->luaMemLimit()) : 0u);
    return fail("oom");
  }

  std::string url = urlIn;
  if (!M4xNetPolicy::isAllowedUrl(url)) return fail("https_required");

  const uint32_t deadline = millis() + static_cast<uint32_t>(timeoutMs);
  std::vector<M4xNetPolicy::ResponseHeader> respHeaders;
  std::vector<M4xNetPolicy::ResponseHeader> accumulatedSetCookies;
  int code = -1;
  std::string err;
  std::string value;

  auto hasHeaderCI = [&](const char* name) {
    for (const auto& hv : headerList) {
      if (M4xNetPolicy::toLowerAscii(hv.first) == M4xNetPolicy::toLowerAscii(name)) return true;
    }
    return false;
  };

  for (int hop = 0; hop <= M4xNetPolicy::kMaxRedirects; ++hop) {
    if (static_cast<int32_t>(deadline - millis()) <= 0) {
      err = "timeout";
      break;
    }
    if (!M4xNetPolicy::isAllowedUrl(url)) {
      err = "https_required";
      break;
    }

    std::unique_ptr<WiFiClient> client;
    {
      auto* secure = new WiFiClientSecure();
      configureTlsClient(secure, h);
      client.reset(secure);
    }

    HTTPClient http;
    http.setTimeout(timeoutMs);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.collectAllHeaders(true);

    if (!http.begin(*client, url.c_str())) {
      err = "http_begin_failed";
      break;
    }

    if (!hasHeaderCI("Referer")) {
      http.addHeader("Referer", "https://weread.qq.com/");
    }
    applyHttpHeaders(http, headerList,
                     "Mozilla/5.0 (Linux; Android 12) AppleWebKit/537.36 M4xWeread/1.0");

    Serial.printf("[M4xNet] extractPsvts GET %s hop=%d\n", url.c_str(), hop);
    const uint32_t requestStartedMs = millis();
    code = http.GET();
    Serial.printf("[WRPERF] stage=tls_http_request ms=%lu budget_ms=%d status=%d hop=%d extract=psvts\n",
                  static_cast<unsigned long>(millis() - requestStartedMs), timeoutMs, code, hop);

    respHeaders.clear();
    if (code > 0) {
      const int hc = http.headers();
      for (int i = 0; i < hc; ++i) {
        M4xNetPolicy::ResponseHeader rh;
        rh.name = http.headerName(i).c_str();
        rh.value = http.header(i).c_str();
        respHeaders.push_back(std::move(rh));
      }
    }

    if (code >= 300 && code < 400) {
      const String loc = http.getLocation();
      const std::string nextPreview =
          loc.length() ? M4xNetPolicy::resolveRedirectUrl(url, loc.c_str()) : std::string();
      const bool nextSame = !nextPreview.empty() && M4xHttp::sameOrigin(url, nextPreview);
      std::vector<std::string> hopCookies;
      if (nextSame) {
        for (const auto& h : respHeaders) {
          if (M4xNetPolicy::toLowerAscii(h.name) == "set-cookie") {
            accumulatedSetCookies.push_back(h);
            hopCookies.push_back(h.value);
          }
        }
      }
      http.end();
      if (loc.length() == 0) {
        err = "redirect_no_location";
        break;
      }
      const std::string next = nextPreview;
      if (next.empty() || !M4xNetPolicy::isAllowedUrl(next)) {
        err = "redirect_blocked";
        break;
      }
      const bool same = nextSame;
      headerList = M4xNetPolicy::headersForRedirect(headerList, same);
      if (same && !hopCookies.empty()) {
        M4xNetPolicy::mergeSetCookiesIntoRequestHeaders(headerList, hopCookies);
      }
      url = next;
      if (hop == M4xNetPolicy::kMaxRedirects) {
        err = "too_many_redirects";
        break;
      }
      continue;
    }

    if (code > 0) {
      NetworkClient* stream = http.getStreamPtr();
      if (stream) {
        const int contentLen = http.getSize();
        M4xHttp::BodyKind kind;
        size_t clen = 0;
        if (headerIsChunked(respHeaders)) {
          kind = M4xHttp::BodyKind::Chunked;
        } else if (contentLen >= 0) {
          kind = M4xHttp::BodyKind::ContentLength;
          clen = static_cast<size_t>(contentLen);
        } else {
          kind = M4xHttp::BodyKind::UntilClose;
        }

        M4xHttp::Limits lim;
        // maxBody unused for scan (scan uses its own maxScan); keep set for consistency.
        lim.maxBody = M4xPsvts::kMaxScanBytes;
        lim.totalTimeoutMs = static_cast<uint32_t>(timeoutMs);
        lim.idleTimeoutMs = 5000;
        lim.nowMs = &httpNowMs;
        lim.onWait = &httpWaitDelay;
        lim.isCancelled = &httpIsCancelled;

        WifiHttpStream wstream(stream);
        const M4xPsvts::Result sr =
            M4xPsvts::scanBody(wstream, kind, clen, lim, M4xPsvts::kMaxScanBytes, M4xPsvts::kMaxValueLen);
        if (sr.ok) {
          value = sr.value;
          err.clear();
        } else {
          err = sr.error.empty() ? "psvts_not_found" : sr.error;
        }
        Serial.printf("[M4xNet] extractPsvts status=%d scanned=%u found=%d err=%s\n", code,
                      static_cast<unsigned>(sr.scanned), sr.found ? 1 : 0, err.c_str());
      } else {
        err = "no_stream";
      }
    } else {
      err = http.errorToString(code).c_str();
    }
    http.end();
    for (const auto& sc : accumulatedSetCookies) {
      respHeaders.push_back(sc);
    }
    break;
  }

  const bool httpOk = (code >= 200 && code < 300);
  const bool ok = httpOk && err.empty() && !value.empty();
  if (httpOk && err.empty() && value.empty()) err = "psvts_not_found";
  pushNetPsvtsResult(L, ok, code, value, err.c_str(), respHeaders);
  return 1;
}

// ---- json ----

void pushJsonVariant(lua_State* L, JsonVariantConst v);

void pushJsonObject(lua_State* L, JsonObjectConst obj) {
  lua_newtable(L);
  for (JsonPairConst kv : obj) {
    lua_pushstring(L, kv.key().c_str());
    pushJsonVariant(L, kv.value());
    lua_settable(L, -3);
  }
}

void pushJsonArray(lua_State* L, JsonArrayConst arr) {
  lua_newtable(L);
  int i = 1;
  for (JsonVariantConst v : arr) {
    pushJsonVariant(L, v);
    lua_rawseti(L, -2, i++);
  }
}

void pushJsonVariant(lua_State* L, JsonVariantConst v) {
  if (v.isNull()) {
    lua_pushnil(L);
  } else if (v.is<bool>()) {
    lua_pushboolean(L, v.as<bool>() ? 1 : 0);
  } else if (v.is<long long>() || v.is<int>() || v.is<long>()) {
    lua_pushnumber(L, static_cast<lua_Number>(v.as<double>()));
  } else if (v.is<double>() || v.is<float>()) {
    lua_pushnumber(L, static_cast<lua_Number>(v.as<double>()));
  } else if (v.is<const char*>()) {
    lua_pushstring(L, v.as<const char*>());
  } else if (v.is<JsonObjectConst>()) {
    pushJsonObject(L, v.as<JsonObjectConst>());
  } else if (v.is<JsonArrayConst>()) {
    pushJsonArray(L, v.as<JsonArrayConst>());
  } else {
    // Fallback: serialize scalar-ish
    const char* s = v.as<const char*>();
    if (s) lua_pushstring(L, s);
    else lua_pushnil(L);
  }
}

// Defined below with the ESP32 PSRAM allocator; keeping the declaration here
// lets the generic Lua JSON helper use the same off-chip working memory as
// dl.jsonGet instead of silently consuming the scarce internal heap.
#if defined(ARDUINO_ARCH_ESP32)
static ArduinoJson::Allocator* PsramJsonAllocatorInstance();
#endif

int l_json_decode(lua_State* L) {
  size_t len = 0;
  const char* s = luaL_checklstring(L, 1, &len);
  if (!s) {
    lua_pushnil(L);
    lua_pushstring(L, "nil");
    return 2;
  }
  // Safety: ArduinoJson pools ~1.5-2x the input in the C++ heap (PSRAM), then
  // Lua copies the strings into the 512KB sandbox heap. The input string is
  // ALREADY in the Lua heap (net.request body), so decoding needs roughly one
  // more copy of it plus table overhead. Reject when the remaining headroom
  // (already excluding the input) cannot hold input + 32KB margin.
  if (gHost) {
    const size_t headroom = gHost->luaMemHeadroom();
    if (headroom < 32768 + 4096 || len + 32768 > headroom) {
      lua_pushnil(L);
      lua_pushstring(L, "json_too_large");
      return 2;
    }
  }
  // Dynamic document; capacity ~ body (ArduinoJson 7 grows).
  JsonDocument doc(
#if defined(ARDUINO_ARCH_ESP32)
      PsramJsonAllocatorInstance()
#else
      ArduinoJson::detail::DefaultAllocator::instance()
#endif
  );
  const DeserializationError err = deserializeJson(doc, s, len);
  if (err) {
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  pushJsonVariant(L, doc.as<JsonVariantConst>());
  return 1;
}

bool encodeLuaValue(lua_State* L, int idx, JsonVariant parent, const char* key, bool isArray, int arrIndex);

bool encodeLuaTable(lua_State* L, int idx, JsonVariant dest) {
  // Heuristic: array if consecutive integer keys from 1.
  const int absIdx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
  bool isArray = true;
  int maxIndex = 0;
  int count = 0;
  lua_pushnil(L);
  while (lua_next(L, absIdx) != 0) {
    if (!lua_isnumber(L, -2)) {
      isArray = false;
    } else {
      const int i = static_cast<int>(lua_tonumber(L, -2));
      if (i < 1) isArray = false;
      if (i > maxIndex) maxIndex = i;
    }
    count++;
    lua_pop(L, 1);
  }
  if (isArray && count > 0 && maxIndex == count) {
    JsonArray arr = dest.to<JsonArray>();
    for (int i = 1; i <= maxIndex; i++) {
      lua_rawgeti(L, absIdx, i);
      JsonVariant el = arr.add<JsonVariant>();
      if (!encodeLuaValue(L, -1, el, nullptr, true, i)) {
        lua_pop(L, 1);
        return false;
      }
      lua_pop(L, 1);
    }
    return true;
  }
  JsonObject obj = dest.to<JsonObject>();
  lua_pushnil(L);
  while (lua_next(L, absIdx) != 0) {
    if (!lua_isstring(L, -2) && !lua_isnumber(L, -2)) {
      lua_pop(L, 1);
      continue;
    }
    const char* k = lua_tostring(L, -2);
    if (!k) {
      lua_pop(L, 1);
      continue;
    }
    JsonVariant child = obj[k].to<JsonVariant>();
    if (!encodeLuaValue(L, -1, child, k, false, 0)) {
      lua_pop(L, 1);
      return false;
    }
    lua_pop(L, 1);
  }
  return true;
}

bool encodeLuaValue(lua_State* L, int idx, JsonVariant dest, const char* /*key*/, bool /*isArray*/,
                    int /*arrIndex*/) {
  const int t = lua_type(L, idx);
  switch (t) {
    case LUA_TNIL:
      dest.set(nullptr);
      return true;
    case LUA_TBOOLEAN:
      dest.set(lua_toboolean(L, idx) != 0);
      return true;
    case LUA_TNUMBER:
      dest.set(lua_tonumber(L, idx));
      return true;
    case LUA_TSTRING:
      dest.set(lua_tostring(L, idx));
      return true;
    case LUA_TTABLE:
      return encodeLuaTable(L, idx, dest);
    default:
      dest.set(nullptr);
      return true;
  }
}

int l_json_encode(lua_State* L) {
  JsonDocument doc;
  JsonVariant root = doc.to<JsonVariant>();
  if (!encodeLuaValue(L, 1, root, nullptr, false, 0)) {
    lua_pushnil(L);
    lua_pushstring(L, "encode_failed");
    return 2;
  }
  std::string out;
  const size_t n = measureJson(doc);
  out.resize(n + 1);
  const size_t written = serializeJson(doc, &out[0], n + 1);
  lua_pushlstring(L, out.data(), written);
  return 1;
}

// ---- crypto / weread ----

int l_crypto_md5(lua_State* L) {
  size_t len = 0;
  const char* s = luaL_checklstring(L, 1, &len);
  const std::string hex = weread_crypto::md5Hex(reinterpret_cast<const uint8_t*>(s), len);
  lua_pushlstring(L, hex.data(), hex.size());
  return 1;
}

int l_crypto_urlEncode(lua_State* L) {
  size_t len = 0;
  const char* s = luaL_checklstring(L, 1, &len);
  const std::string out = weread_crypto::urlEncode(std::string(s, len));
  lua_pushlstring(L, out.data(), out.size());
  return 1;
}

int l_weread_e(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  const std::string out = weread_crypto::e(s ? s : "");
  lua_pushlstring(L, out.data(), out.size());
  return 1;
}

int l_weread_sign(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  const std::string out = weread_crypto::sign(s ? s : "");
  lua_pushlstring(L, out.data(), out.size());
  return 1;
}

// weread.sortedQuery(params_table) -> fullQueryWithS, queryForSign
// params_table: { {k,v}, ... } or { k = v, ... }
int l_weread_sortedQuery(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  std::vector<std::pair<std::string, std::string>> params;

  // Prefer array of {key, value} pairs; also accept map.
  const size_t len = lua_rawlen(L, 1);
  if (len > 0) {
    for (size_t i = 1; i <= len; i++) {
      lua_rawgeti(L, 1, static_cast<int>(i));
      if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        if (lua_isstring(L, -2) && (lua_isstring(L, -1) || lua_isnumber(L, -1))) {
          params.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
        }
        lua_pop(L, 2);
      }
      lua_pop(L, 1);
    }
  } else {
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
      if (lua_isstring(L, -2) && (lua_isstring(L, -1) || lua_isnumber(L, -1))) {
        params.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
      }
      lua_pop(L, 1);
    }
  }

  std::string forSign;
  const std::string full = weread_crypto::sortedQueryWithSign(params, forSign);
  lua_pushlstring(L, full.data(), full.size());
  lua_pushlstring(L, forSign.data(), forSign.size());
  return 2;
}

// weread.makeContentParams(bookId, chapterUid, psvts [, style=false, sc=1]) -> json body
int l_weread_makeContentParams(lua_State* L) {
  const char* bookId = luaL_checkstring(L, 1);
  const char* chapterUid = luaL_checkstring(L, 2);
  const char* psvts = luaL_checkstring(L, 3);
  const bool style = lua_toboolean(L, 4) != 0;
  const int sc = lua_isnoneornil(L, 5) ? 1 : static_cast<int>(luaL_checknumber(L, 5));
  const long now = static_cast<long>(time(nullptr));
  const long rnd = static_cast<long>(esp_random() % 10000);
  const std::string j = weread_crypto::makeContentParamsJson(bookId ? bookId : "", chapterUid ? chapterUid : "",
                                                             psvts ? psvts : "", style, sc, now, rnd);
  lua_pushlstring(L, j.data(), j.size());
  return 1;
}

// weread.decodeShards(s0, s1 [, s2]) -> plaintext or nil
int l_weread_decodeShards(lua_State* L) {
  size_t n0 = 0, n1 = 0, n2 = 0;
  const char* s0 = luaL_optlstring(L, 1, "", &n0);
  const char* s1 = luaL_optlstring(L, 2, "", &n1);
  const char* s2 = luaL_optlstring(L, 3, "", &n2);
  const std::string plain =
      weread_crypto::decodeContentShards(std::string(s0, n0), std::string(s1, n1), std::string(s2, n2));
  if (plain.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, plain.data(), plain.size());
  return 1;
}

int l_weread_extractPsvts(lua_State* L) {
  size_t n = 0;
  const char* html = luaL_checklstring(L, 1, &n);
  const std::string p = weread_crypto::extractPsvts(std::string(html, n));
  if (p.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, p.data(), p.size());
  return 1;
}

int l_weread_stripXhtml(lua_State* L) {
  size_t n = 0;
  const char* xhtml = luaL_checklstring(L, 1, &n);
  const std::string t = weread_crypto::stripXhtml(std::string(xhtml, n));
  lua_pushlstring(L, t.data(), t.size());
  return 1;
}

// ---- Host-owned list scene (ui.list*) ----------------------------------
// Plugins describe a list declaratively; the host owns rendering, pagination,
// footer buttons and input mapping. Lua only receives row/page/back callbacks.

namespace {

constexpr size_t kUiSceneMaxRows = 4096;

class SdUiFileInput final : public M4FileRows::ISeekableInput {
 public:
  explicit SdUiFileInput(const char* path) { opened_ = SdMan.openFileForRead("UIF", path, file_); }
  ~SdUiFileInput() override { file_.close(); }
  bool opened() const { return opened_; }
  uint64_t size() const override { return opened_ ? file_.fileSize() : 0; }
  bool seek(uint64_t offset) override { return opened_ && file_.seek(offset); }
  int64_t read(uint8_t* dst, size_t capacity) override {
    if (!opened_) return -1;
    const int n = file_.read(dst, capacity);
    return n < 0 ? -1 : n;
  }

 private:
  FsFile file_;
  bool opened_ = false;
};

bool uiRowsFromLua(lua_State* L, M4xLuaHost::UiListScene& sc) {
  sc.rows.clear();
  sc.rowSubs.clear();
  sc.hasSubtitles = false;
  if (!lua_istable(L, -1)) return false;
  const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
  if (n > kUiSceneMaxRows) return false;
  sc.rows.reserve(n);
  sc.rowSubs.reserve(n);
  for (size_t i = 1; i <= n; ++i) {
    lua_rawgeti(L, -1, static_cast<int>(i));
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "title");
      sc.rows.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
      lua_pop(L, 1);
      lua_getfield(L, -1, "sub");
      sc.rowSubs.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
      if (!sc.rowSubs.back().empty()) sc.hasSubtitles = true;
      lua_pop(L, 1);
    } else if (lua_isstring(L, -1)) {
      sc.rows.push_back(lua_tostring(L, -1));
      sc.rowSubs.push_back("");
    } else {
      sc.rows.push_back("");
      sc.rowSubs.push_back("");
    }
    lua_pop(L, 1);
  }
  return true;
}

}  // namespace

// ui.listOpen({title, footer, rows=[{title,sub}|string...], page_size,
//              page_count, initial_page, remote_page, on_row, on_page, on_back})
//              -> true | false, err
int l_ui_listOpen(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "scene_required");
    return 2;
  }
  M4xLuaHost::UiListScene sc;
  lua_getfield(L, 1, "title");
  if (lua_isstring(L, -1)) sc.title = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "footer");
  if (lua_isstring(L, -1)) sc.footer = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "page_size");
  if (lua_isnumber(L, -1)) sc.pageSize = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "page_count");
  if (lua_isnumber(L, -1)) sc.pageCountOverride = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  int initialPage = 1;
  lua_getfield(L, 1, "initial_page");
  if (lua_isnumber(L, -1)) initialPage = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "remote_page");
  sc.remotePage = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  lua_getfield(L, 1, "on_row");
  if (lua_isstring(L, -1)) sc.onRow = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "on_page");
  if (lua_isstring(L, -1)) sc.onPage = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "on_back");
  if (lua_isstring(L, -1)) sc.onBack = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "rows");
  if (!uiRowsFromLua(L, sc)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_rows");
    return 2;
  }
  lua_pop(L, 1);
  if (sc.pageSize < 3) sc.pageSize = 12;
  if (sc.pageSize > 40) sc.pageSize = 40;
  sc.page = std::max(1, initialPage);
  if (sc.pageCountOverride < 1) sc.pageCountOverride = 0;
  if (sc.pageCountOverride > 4096) sc.pageCountOverride = 4096;
  if (sc.pageCountOverride > 0) sc.page = std::min(sc.page, sc.pageCountOverride);
  sc.active = true;
  sc.repaint = true;
  sc.generation++;
  h->uiScene() = std::move(sc);
  lua_pushboolean(L, 1);
  return 1;
}

// ui.listOpenFile({title, source="rel", page_size, footer, on_row, on_page, on_back})
//   -> true | false, err
// File-backed list: `source` is an app-data-relative text file (one tab-
// separated record per line, written by dl.jsonToFile).  The first field is
// an internal key and the second is the user-facing label; the key is kept
// out of the rendered UI but is still passed to the row callback.  The host
// scans it with M4FileRowSource and materializes only the active page.
// Catalog rows are `key<TAB>label`; this is intentionally a bounded SD source,
// not a Lua-resident copy of the complete directory.
int l_ui_listOpenFile(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "scene_required");
    return 2;
  }
  if (!hasPerm(h->app_, "filesystem.appdata")) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "permission denied: filesystem.appdata");
    return 2;
  }
  // SdSeekableInput performs SdMan.openFileForRead only after this gate;
  // keeping the permission boundary explicit prevents probing app data.
  // FileRowSource enforces maxFileBytes = 8u * 1024u * 1024u and maxRows = 200000.
  lua_getfield(L, 1, "source");
  const char* source = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  if (!source || !source[0]) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no_source");
    return 2;
  }
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir().c_str(), source, abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_path");
    return 2;
  }
  M4xLuaHost::UiListScene sc;
  lua_getfield(L, 1, "title");
  if (lua_isstring(L, -1)) sc.title = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "footer");
  if (lua_isstring(L, -1)) sc.footer = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "page_size");
  if (lua_isnumber(L, -1)) sc.pageSize = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "on_row");
  if (lua_isstring(L, -1)) sc.onRow = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "on_page");
  if (lua_isstring(L, -1)) sc.onPage = lua_tostring(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "on_back");
  if (lua_isstring(L, -1)) sc.onBack = lua_tostring(L, -1);
  lua_pop(L, 1);
  if (sc.pageSize < 3) sc.pageSize = 12;
  if (sc.pageSize > 40) sc.pageSize = 40;
  const int w = h->renderer_ ? h->renderer_->getScreenWidth() : 480;
  const int height = h->renderer_ ? h->renderer_->getScreenHeight() : 800;
  const M4UiStyle::Theme style = M4UiStyleAdapter::current(w, height);
  const int sourcePageSize = M4xUiList::effectivePageSize(sc.pageSize, style.list.visibleRows);
  auto input = std::make_unique<SdSeekableInput>(abs.c_str());
  if (!input->opened()) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "source_not_found");
    return 2;
  }
  auto sourceRows = std::make_shared<M4FileRows::FileRowSource>();
  // source->open is deliberately performed through the bounded row source;
  // it never materializes the complete catalog in Lua.
  const M4FileRows::Error openError = sourceRows->open(std::move(input), sourcePageSize);
  if (openError != M4FileRows::Error::None || sourceRows->rowCount() == 0) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, openError == M4FileRows::Error::None ? "empty_source"
                                                            : M4FileRows::errorKey(openError));
    return 2;
  }
  sc.fromFile = true;
  sc.fileSource = std::move(sourceRows);
  sc.page = 1;
  sc.active = true;
  sc.repaint = true;
  sc.generation++;
  h->uiScene() = std::move(sc);
  lua_pushboolean(L, 1);
  return 1;
}

// ui.listSetRows(rows [, keepFirstRowIndex]) — replace all rows. Page is
// clamped; with keepFirstRowIndex the first visible row stays put.
int l_ui_listSetRows(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  auto& sc = h->uiScene();
  if (!sc.active) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no_scene");
    return 2;
  }
  if (sc.fromFile) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "file_source_immutable");
    return 2;
  }
  M4xLuaHost::UiListScene next = sc;
  if (!uiRowsFromLua(L, next)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_rows");
    return 2;
  }
  const M4UiStyle::Theme nextStyle = uiSceneStyleFor(*h, next.hasSubtitles);
  const int effectivePageSize = M4xUiList::effectivePageSize(next.pageSize, nextStyle.list.visibleRows);
  const size_t firstIdx = M4xUiList::rowStart(next.page, next.pageSize, nextStyle.list.visibleRows);
  if (next.remotePage) {
    // The resident vector is the current remote page, not the complete
    // catalog. Keep the logical page supplied by the provider.
    if (next.pageCountOverride > 0) {
      next.page = M4xUiList::clampPage(next.page, next.pageCountOverride);
    } else {
      next.page = std::max(1, next.page);
    }
  } else if (lua_gettop(L) >= 2 && lua_isnumber(L, 2)) {
    const size_t keep = static_cast<size_t>(std::max<lua_Integer>(0, lua_tointeger(L, 2)));
    next.page = M4xUiList::clampPage(static_cast<int>(keep / static_cast<size_t>(effectivePageSize)) + 1,
                                     M4xUiList::totalPages(next.rows.size(), next.pageSize,
                                                            nextStyle.list.visibleRows));
  } else {
    next.page = M4xUiList::clampPage(
        static_cast<int>(firstIdx / static_cast<size_t>(effectivePageSize)) + 1,
        M4xUiList::totalPages(next.rows.size(), next.pageSize, nextStyle.list.visibleRows));
  }
  next.repaint = true;
  next.generation++;
  h->uiScene() = std::move(next);
  lua_pushboolean(L, 1);
  return 1;
}

// ui.listAppendRows(rows) — grow the tail; current page keeps its screenful.
int l_ui_listAppendRows(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  auto& sc = h->uiScene();
  if (!sc.active) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no_scene");
    return 2;
  }
  if (sc.fromFile) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "file_source_immutable");
    return 2;
  }
  M4xLuaHost::UiListScene add;
  if (!uiRowsFromLua(L, add)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_rows");
    return 2;
  }
  for (size_t i = 0; i < add.rows.size(); ++i) {
    sc.rows.push_back(add.rows[i]);
    sc.rowSubs.push_back(add.rowSubs[i]);
  }
  sc.hasSubtitles = sc.hasSubtitles || add.hasSubtitles;
  if (!sc.remotePage) {
    sc.page = M4xUiList::clampPage(sc.page,
                                  M4xUiList::totalPages(sc.rows.size(), sc.pageSize,
                                                        uiSceneStyle(*h).list.visibleRows));
  }
  sc.repaint = true;
  lua_pushboolean(L, 1);
  return 1;
}

// ui.listPrependRows(rows) — grow the head; the window shifts so the previous
// first screenful stays visible.
int l_ui_listPrependRows(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  auto& sc = h->uiScene();
  if (!sc.active) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no_scene");
    return 2;
  }
  if (sc.fromFile) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "file_source_immutable");
    return 2;
  }
  M4xLuaHost::UiListScene add;
  if (!uiRowsFromLua(L, add)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_rows");
    return 2;
  }
  sc.rows.insert(sc.rows.begin(), add.rows.begin(), add.rows.end());
  sc.rowSubs.insert(sc.rowSubs.begin(), add.rowSubs.begin(), add.rowSubs.end());
  sc.hasSubtitles = sc.hasSubtitles || add.hasSubtitles;
  if (!sc.remotePage) {
    sc.page = M4xUiList::pageAfterPrepend(sc.page, add.rows.size(), sc.pageSize);
    sc.page = M4xUiList::clampPage(sc.page,
                                  M4xUiList::totalPages(sc.rows.size(), sc.pageSize,
                                                        uiSceneStyle(*h).list.visibleRows));
  }
  sc.repaint = true;
  lua_pushboolean(L, 1);
  return 1;
}

// ui.listClose() — host hands the display back to Lua painting.
int l_ui_listClose(lua_State* L) {
  auto* h = hostFromLua(L);
  if (h) h->closeUiScene();
  return 0;
}

// ui.listPage() -> page, totalPages
int l_ui_listPage(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h || !h->uiScene().active) {
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 2;
  }
  const auto& sc = h->uiScene();
  const int w = h->renderer_ ? h->renderer_->getScreenWidth() : 480;
  const int height = h->renderer_ ? h->renderer_->getScreenHeight() : 800;
  const M4UiStyle::Theme style = uiSceneStyle(*h);
  lua_pushnumber(L, sc.page);
  const size_t rowCount = uiSceneRowCount(sc);
  lua_pushnumber(L, sc.pageCountOverride > 0
      ? sc.pageCountOverride
      : M4xUiList::totalPages(rowCount, sc.pageSize, style.list.visibleRows));
  return 2;
}

// ---- dl: safe download abstraction --------------------------------------
// dl.download({url, headers={...}, path="rel", max_bytes, timeout_ms}) -> table
// Enforces: https-only, size cap, wall timeout; streams the body straight to
// SD (.part -> atomic rename) so large payloads never touch the Lua heap.
// Returns {ok=true, size, sha256} or {ok=false, error}.
namespace {

class SdTransactionBackend final : public M4xHostIo::TransactionBackend {
 public:
  SdTransactionBackend(std::string live, const char* tag)
      : live_(std::move(live)), part_(live_ + ".part"), bak_(live_ + ".dlbak"), tag_(tag) {}

  bool openPart() override {
    if (opened_) return false;
    // Streamed downloads commonly target app-data subdirectories (for
    // example cache/<bookId>/toc_rows.txt).  fs.writeFile/replaceFile create
    // parents, but the transaction backend is also used by dl.download and
    // dl.jsonToFile and must provide the same contract.  Without this mkdir
    // the first directory fetch on a fresh plugin install fails with the
    // misleading `sd_open_failed` even though the SD card is healthy.
    const size_t slash = live_.rfind('/');
    if (slash != std::string::npos && slash > 0) {
      const std::string parent = live_.substr(0, slash);
      if (!SdMan.mkdir(parent.c_str(), true) && !SdMan.exists(parent.c_str())) {
        return false;
      }
    }
    // Recover a reset between live->bak and part->live before starting a new
    // transaction. If live exists, publish completed and stale bak may go.
    if (SdMan.exists(bak_.c_str())) {
      if (!SdMan.exists(live_.c_str())) {
        if (!SdMan.rename(bak_.c_str(), live_.c_str())) return false;
      } else {
        (void)SdMan.remove(bak_.c_str());
      }
    }
    if (SdMan.exists(part_.c_str())) (void)SdMan.remove(part_.c_str());
    opened_ = SdMan.openFileForWrite(tag_, part_.c_str(), file_);
    return opened_;
  }
  size_t writePart(const uint8_t* data, size_t len) override {
    return opened_ ? file_.write(data, len) : 0;
  }
  bool syncPart() override { return opened_ && file_.sync(); }
  bool closePart() override {
    if (opened_) file_.close();
    opened_ = false;
    return true;
  }
  bool partSize(size_t& sizeOut) override {
    FsFile check;
    if (!SdMan.openFileForRead(tag_, part_.c_str(), check)) return false;
    sizeOut = static_cast<size_t>(check.fileSize());
    check.close();
    return true;
  }
  bool publishPart() override {
    const bool hadLive = SdMan.exists(live_.c_str());
    if (SdMan.exists(bak_.c_str()) && !SdMan.remove(bak_.c_str())) return false;
    if (hadLive && !SdMan.rename(live_.c_str(), bak_.c_str())) return false;
    if (!SdMan.rename(part_.c_str(), live_.c_str())) {
      if (hadLive && SdMan.exists(bak_.c_str())) (void)SdMan.rename(bak_.c_str(), live_.c_str());
      return false;
    }
    if (hadLive && SdMan.exists(bak_.c_str())) (void)SdMan.remove(bak_.c_str());
    return true;
  }
  void discardPart() override {
    if (opened_) file_.close();
    opened_ = false;
    if (SdMan.exists(part_.c_str())) (void)SdMan.remove(part_.c_str());
    // A failed publish restores bak above. If reset happened between renames,
    // leave .dlbak for explicit recovery rather than deleting the only copy.
  }

 private:
  std::string live_;
  std::string part_;
  std::string bak_;
  const char* tag_;
  FsFile file_;
  bool opened_ = false;
};

class TransactionJsonSink final : public M4xJsonStream::Sink {
 public:
  explicit TransactionJsonSink(M4xHostIo::TransactionalWriter& writer) : writer_(writer) {
#if defined(ARDUINO_ARCH_ESP32)
    buffer_ = static_cast<uint8_t*>(heap_caps_malloc(kBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
  }
  ~TransactionJsonSink() override {
#if defined(ARDUINO_ARCH_ESP32)
    if (buffer_) heap_caps_free(buffer_);
#endif
  }
  bool write(const uint8_t* data, size_t len) override {
    if (!data && len) return false;
    while (len) {
      if (!buffer_ || len >= kBufferBytes) {
        if (!flush() || !writer_.write(data, len)) return false;
        return true;
      }
      const size_t n = std::min(len, kBufferBytes - used_);
      std::memcpy(buffer_ + used_, data, n);
      used_ += n;
      data += n;
      len -= n;
      if (used_ == kBufferBytes && !flush()) return false;
    }
    return true;
  }
  bool flush() {
    if (!used_) return true;
    if (!buffer_ || !writer_.write(buffer_, used_)) return false;
    used_ = 0;
    return true;
  }

 private:
  static constexpr size_t kBufferBytes = 16 * 1024;
  M4xHostIo::TransactionalWriter& writer_;
  uint8_t* buffer_ = nullptr;
  size_t used_ = 0;
};

class NetworkBodySource final : public M4xHostIo::StreamSource {
 public:
  explicit NetworkBodySource(NetworkClient* stream, bool chunked = false)
      : stream_(stream), chunked_(chunked) {}
  int read(uint8_t* data, size_t capacity) override {
    if (!stream_) return -2;
    if (chunked_) return readChunked(data, capacity);
    const int avail = stream_->available();
    if (avail > 0) {
      const size_t want = std::min<size_t>(capacity, static_cast<size_t>(avail));
      const int n = stream_->read(data, static_cast<int>(want));
      return n < 0 ? -2 : n;
    }
    return stream_->connected() ? 0 : -1;
  }

 private:
  // http.getStreamPtr() returns the RAW TCP stream — chunked responses keep
  // their `1a2b\r\n...\r\n` framing. jjwxc app-cdn sends chunked+keep-alive;
  // feeding the framing to ArduinoJson yielded InvalidInput on every
  // category fetch (head=0x35 tail=0x0a). Decode chunk framing here and
  // report a true -1 EOF at the 0-length chunk (no idle-timeout guessing).
  int readChunked(uint8_t* data, size_t capacity) {
    if (done_) return -1;
    for (;;) {
      if (trailerRemain_ > 0) {
        while (trailerRemain_ > 0) {
          const int c = stream_->read();
          if (c < 0) return 0;  // WANT_READ: wait for more
          --trailerRemain_;
        }
        continue;
      }
      if (chunkRemain_ > 0) {
        const size_t want = std::min(capacity, chunkRemain_);
        const int n = stream_->read(data, static_cast<int>(want));
        if (n < 0) return 0;  // TLS WANT_READ
        if (n == 0) return 0;
        chunkRemain_ -= static_cast<size_t>(n);
        if (chunkRemain_ == 0) trailerRemain_ = 2;  // \r\n after chunk data
        return n;
      }
      // Read a chunk-size line.
      int c = stream_->read();
      if (c < 0) return 0;
      if (c == '\n') {
        size_t hn = hlen_;
        while (hn && (header_[hn - 1] == '\r' || header_[hn - 1] == ' ')) --hn;
        hlen_ = 0;
        size_t size = 0;
        for (size_t i = 0; i < hn; ++i) {
          const char ch = static_cast<char>(header_[i]);
          unsigned v;
          if (ch >= '0' && ch <= '9') v = static_cast<unsigned>(ch - '0');
          else if (ch >= 'a' && ch <= 'f') v = static_cast<unsigned>(ch - 'a' + 10);
          else if (ch >= 'A' && ch <= 'F') v = static_cast<unsigned>(ch - 'A' + 10);
          else return -2;
          size = size * 16 + v;
        }
        if (size == 0) {
          done_ = true;
          return -1;  // last chunk: clean EOF
        }
        chunkRemain_ = size;
        continue;
      }
      if (hlen_ < sizeof(header_) - 1) header_[hlen_++] = static_cast<uint8_t>(c);
    }
  }

  NetworkClient* stream_;
  bool chunked_ = false;
  bool done_ = false;
  size_t chunkRemain_ = 0;
  size_t trailerRemain_ = 0;
  uint8_t header_[40];
  size_t hlen_ = 0;
};

class HashTransactionSink final : public M4xHostIo::StreamSink {
 public:
  HashTransactionSink(M4xHostIo::TransactionalWriter& writer, mbedtls_sha256_context& sha)
      : writer_(writer), sha_(sha) {}
  bool write(const uint8_t* data, size_t len) override {
    if (!writer_.write(data, len)) return false;
    mbedtls_sha256_update(&sha_, data, len);
    return true;
  }
 private:
  M4xHostIo::TransactionalWriter& writer_;
  mbedtls_sha256_context& sha_;
};

class JsonExtractorStreamSink final : public M4xHostIo::StreamSink {
 public:
  explicit JsonExtractorStreamSink(M4xJsonStream::RecordExtractor& extractor) : extractor_(extractor) {}
  bool write(const uint8_t* data, size_t len) override { return extractor_.feed(data, len); }
 private:
  M4xJsonStream::RecordExtractor& extractor_;
};

class NetBodyStreamSink final : public M4xHostIo::StreamSink {
 public:
  NetBodyStreamSink(NetBodyBuf& body, size_t cap) : body_(body), cap_(cap) {}
  bool write(const uint8_t* data, size_t len) override {
    return body_.append(reinterpret_cast<const char*>(data), len, cap_);
  }
 private:
  NetBodyBuf& body_;
  size_t cap_;
};

M4xHostIo::StreamRuntime hostStreamRuntime() {
  return M4xHostIo::StreamRuntime{httpNowMs, httpWaitDelay, httpIsCancelled};
}

bool dlStreamToFile(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                    const std::string& absPath, size_t maxBytes, uint32_t timeoutMs, size_t& outSize,
                    char shaHex[65], std::string& err) {
  uint8_t digest[32] = {0};
  auto* secure = new WiFiClientSecure();
  configureTlsClient(secure, nullptr);  // no app context: bundle verification
  std::unique_ptr<WiFiClient> client(secure);

  HTTPClient http;
  http.setTimeout(static_cast<int>(timeoutMs));
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.collectAllHeaders(true);
  if (!http.begin(*client, url.c_str())) {
    err = "http_begin_failed";
    return false;
  }
  applyHttpHeaders(http, headers, "Mozilla/5.0 (Linux; Android 12) AppleWebKit/537.36 M4xApp/1.0");
  const int code = http.GET();
  if (code < 0) {
    err = "http_request_failed";
    http.end();
    return false;
  }
  if (code >= 300 && code < 400) {
    err = "redirect_unsupported";
    http.end();
    return false;
  }
  if (code != HTTP_CODE_OK) {
    err = "http_" + std::to_string(code);
    http.end();
    return false;
  }
  const int cl = http.getSize();
  if (cl > 0 && static_cast<size_t>(cl) > maxBytes) {
    err = "response_too_large";
    http.end();
    return false;
  }
  SdTransactionBackend backend(absPath, "DL");
  M4xHostIo::TransactionalWriter writer(backend);
  if (!writer.begin()) {
    err = "sd_open_failed";
    http.end();
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  const String teHeader = http.header("Transfer-Encoding");
  const bool chunked = teHeader.length() > 0 && teHeader.indexOf("chunked") >= 0;
  NetworkBodySource source(http.getStreamPtr(), chunked);
  HashTransactionSink sink(writer, ctx);
  const size_t expected = cl >= 0 ? static_cast<size_t>(cl) : static_cast<size_t>(-1);
  const M4xHostIo::StreamResult transferred =
      M4xHostIo::stream(source, sink, maxBytes, expected, timeoutMs, hostStreamRuntime());
  const size_t got = transferred.bytes;
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  http.end();
  if (!transferred.ok()) {
    err = transferred.error == M4xHostIo::StreamError::Sink
              ? "sd_write_failed"
              : M4xHostIo::streamErrorString(transferred.error);
    writer.abort();
    return false;
  }
  // Convert digest to hex.
  for (int i = 0; i < 32; ++i) sprintf(shaHex + i * 2, "%02x", digest[i]);
  shaHex[64] = 0;
  if (!writer.commit()) {
    err = "sd_commit_failed";
    return false;
  }
  outSize = got;
  return true;
}

}  // namespace

#if defined(ARDUINO_ARCH_ESP32)
// ArduinoJson's default allocator mallocs from internal RAM.  A fanqie
// category page parses an ~90KB body into a ~150-200KB pool; on the device's
// ~287KB internal heap that nearly exhausts it and panics the app after
// sustained paging (free heap dropped to ~38KB).  Keep the pool in PSRAM,
// like the Lua VM, so parsing never starves TLS/WiFi buffers.
class PsramJsonAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  void deallocate(void* ptr) override {
    if (ptr) heap_caps_free(ptr);
  }
  void* reallocate(void* ptr, size_t new_size) override {
    void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_realloc(ptr, new_size, MALLOC_CAP_8BIT);
  }
};

static ArduinoJson::Allocator* PsramJsonAllocatorInstance() {
  static PsramJsonAllocator alloc;
  return &alloc;
}
#endif

// dl.jsonGet({url, headers={...}, path={...}, fields={...}, file_out,
//             max_bytes, timeout_ms})
//   -> { {field=value,...}, ... } flattened array | nil, error
//   When file_out is set and exactly one scalar field is selected, the host
//   atomically writes that value to the app-data path and returns
//   {ok=true,size=N}.  This keeps large chapter bodies out of the Lua heap.
// Host-side: streams a bounded response into PSRAM, parses with ArduinoJson (never
// copies the whole body into the Lua heap), then walks `path` collecting each
// array element's `fields`. Nested arrays are flattened (chapterListWithVolume
// is an array of volume arrays). This lets plugins consume multi-hundred-KB
// JSON (e.g. fanqie TOCs) that would otherwise overflow the 512KB Lua heap.
int l_dl_jsonGet(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (const char* denied = M4xHostIo::permissionError(hostIoPermissions(h->app_),
                                                       M4xHostIo::Operation::JsonGet)) {
    lua_pushnil(L);
    lua_pushstring(L, denied);
    return 2;
  }
  if (!lua_istable(L, 1)) {
    lua_pushnil(L);
    lua_pushstring(L, "params_required");
    return 2;
  }
  lua_getfield(L, 1, "url");
  const char* url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  const std::string urlIn = url;  // original URL for same-origin mirror retry
  lua_pop(L, 1);
  lua_getfield(L, 1, "max_bytes");
  const size_t maxBytes = lua_isnumber(L, -1) ? static_cast<size_t>(lua_tointeger(L, -1)) : 0;
  lua_pop(L, 1);
  lua_getfield(L, 1, "timeout_ms");
  const uint32_t timeoutMs = lua_isnumber(L, -1) ? static_cast<uint32_t>(lua_tointeger(L, -1)) : 30000;
  lua_pop(L, 1);
  // Public content mirrors occasionally redirect to a short-lived origin.
  // Redirect following is opt-in and forbidden when credentials are present;
  // ordinary WeRead requests therefore retain the strict no-redirect default.
  lua_getfield(L, 1, "follow_redirects");
  const bool followRedirects = lua_isboolean(L, -1) && lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "file_out");
  const std::string fileOut = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);

  if (!fileOut.empty()) {
    if (const char* denied = M4xHostIo::permissionError(
            hostIoPermissions(h->app_), M4xHostIo::Operation::JsonToFile)) {
      lua_pushnil(L);
      lua_pushstring(L, denied);
      return 2;
    }
  }

  std::vector<std::pair<std::string, std::string>> headers;
  lua_getfield(L, 1, "headers");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
        headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  if (followRedirects) {
    for (const auto& hv : headers) {
      if (M4xNetPolicy::isSensitiveRequestHeader(hv.first)) {
        lua_pushnil(L);
        lua_pushstring(L, "redirect_requires_public_headers");
        return 2;
      }
    }
  }
  std::vector<std::string> path;
  lua_getfield(L, 1, "path");
  if (lua_istable(L, -1)) {
    const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
    for (size_t i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, static_cast<int>(i));
      if (lua_isstring(L, -1)) path.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  std::vector<std::string> fields;
  lua_getfield(L, 1, "fields");
  if (lua_istable(L, -1)) {
    const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
    for (size_t i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, static_cast<int>(i));
      if (lua_isstring(L, -1)) fields.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  if (!url || !url[0] || !M4xHostIo::Limits::validHeaders(headers) ||
      !M4xHostIo::Limits::validJsonShape(path, fields) || !M4xNetPolicy::isAllowedUrl(url)) {
    lua_pushnil(L);
    lua_pushstring(L, "bad_params");
    return 2;
  }
  const size_t cap = std::min(M4xHostIo::Limits::bodyCap(maxBytes, M4xHostIo::Operation::JsonGet),
                              netMaxBodyBytes());
  const uint32_t safeTimeout = M4xHostIo::Limits::timeoutMs(timeoutMs);

  // Stream response into a PSRAM-backed buffer. Reuse one keep-alive TLS
  // connection (netTls_/netHttp_): a fresh handshake per request fragments
  // internal RAM with mbedTLS session buffers (the "list too large; back to
  // shelf" OOM), so the 40KB gate below only runs when a new handshake is
  // actually needed. HTTPClient disconnects itself on host change; a dead
  // keep-alive (server/NAT close) is rebuilt once via the retry loop.
  bool insecureRetried = false;
  int attempt = 0;
  int code = -1;
  for (;;) {
    ++attempt;
    const bool needHandshake = !h->netHttp_ || !h->netTls_ || !h->netTls_->connected();
    if (needHandshake) {
      // Reclaim Lua + refuse TLS when internal heap is fragmented (same gate
      // as net.request). Category booklist OOM used to surface as generic
      // http fail. A live keep-alive connection skips this: its mbedTLS
      // buffers already exist and no new contiguous block is required.
      lua_gc(L, LUA_GCCOLLECT, 0);
      const size_t largestInternal =
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (largestInternal < 32 * 1024) {
        Serial.printf("[M4xNet] dl.jsonGet TLS skipped: internal largest=%u free=%u psram=%u lua=%u/%u\n",
                      static_cast<unsigned>(largestInternal),
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                      h ? static_cast<unsigned>(h->luaMemUsed()) : 0u,
                      h ? static_cast<unsigned>(h->luaMemLimit()) : 0u);
        lua_pushnil(L);
        lua_pushstring(L, "oom");
        return 2;
      }
      if (h->netTls_) h->netTls_->stop();
      h->netTls_.reset();
      h->netHttp_.reset();
    }
    if (!h->netHttp_ || !h->netTls_) {
      h->netTls_ = std::make_unique<WiFiClientSecure>();
      h->netHttp_ = std::make_unique<HTTPClient>();
    }
    auto* secure = h->netTls_.get();
    if (insecureRetried) secure->setInsecure();
    configureTlsClient(secure, h);
    HTTPClient& http = *h->netHttp_;
    http.setTimeout(static_cast<int>(safeTimeout));
    http.setFollowRedirects(followRedirects ? HTTPC_FORCE_FOLLOW_REDIRECTS
                                            : HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.collectAllHeaders(true);
    if (!http.begin(*secure, url)) {
      // Same-domain mirror fallback: retry once without cert verification when
      // the chain is valid but absent from the device bundle.  Only applies to
      // explicit follow_redirects requests (public mirrors, no credentials).
      if (followRedirects && !insecureRetried && M4xHttp::sameOrigin(urlIn, url)) {
        insecureRetried = true;
        continue;
      }
      if (attempt == 1) continue;  // reused connection died: rebuild once
      lua_pushnil(L);
      lua_pushstring(L, "http_begin_failed");
      return 2;
    }
    // Extend wall *before* TLS/GET — long category fetches used to sit silent
    // until timeout with dual-UA hangs (jjwxc app-cdn).
    if (h) h->extendCallbackWallMs(safeTimeout + 12000);
    Serial.printf("[M4xNet] dl.jsonGet begin %s\n", url);
    applyHttpHeaders(http, headers, "Mozilla/5.0 (Linux; Android 12) AppleWebKit/537.36 M4xApp/1.0");
    code = http.GET();
    if (code >= 0) break;
    http.end();
    if (followRedirects && !insecureRetried && M4xHttp::sameOrigin(urlIn, url)) {
      insecureRetried = true;
      continue;
    }
    if (attempt == 1) continue;  // stale keep-alive: one rebuild
    Serial.printf("[M4xNet] dl.jsonGet GET fail code=%d\n", code);
    lua_pushnil(L);
    lua_pushstring(L, "http_request_failed");
    return 2;
  }
  if (code >= 300 && code < 400) {
    h->netHttp_->end();
    lua_pushnil(L);
    lua_pushstring(L, "redirect_unsupported");
    return 2;
  }
  if (code != HTTP_CODE_OK) {
    h->netHttp_->end();
    lua_pushnil(L);
    lua_pushstring(L, ("http_" + std::to_string(code)).c_str());
    return 2;
  }
  const int cl = h->netHttp_->getSize();
  if (cl > 0 && static_cast<size_t>(cl) > cap) {
    h->netHttp_->end();
    lua_pushnil(L);
    lua_pushstring(L, "response_too_large");
    return 2;
  }
  Serial.printf("[M4xNet] dl.jsonGet GET ok status=%d cl=%d\n", code, cl);
  const String teHeader = h->netHttp_->header("Transfer-Encoding");
  const bool chunked = teHeader.length() > 0 && teHeader.indexOf("chunked") >= 0;
  NetBodyBuf body;
  const size_t initial = cl > 0 ? static_cast<size_t>(cl) : std::min<size_t>(cap, 16 * 1024);
  if (initial && !body.reserve(initial)) {
    h->netHttp_->end();
    lua_pushnil(L);
    lua_pushstring(L, "out_of_memory");
    return 2;
  }
  NetworkBodySource source(h->netHttp_->getStreamPtr(), chunked);
  NetBodyStreamSink sink(body, cap);
  const size_t expected = cl >= 0 ? static_cast<size_t>(cl) : static_cast<size_t>(-1);
  const M4xHostIo::StreamResult transferred =
      M4xHostIo::stream(source, sink, cap, expected, safeTimeout, hostStreamRuntime());
  h->netHttp_->end();
  if (!transferred.ok()) {
    const char* err = transferred.error == M4xHostIo::StreamError::Sink
                          ? "response_too_large_or_oom"
                          : M4xHostIo::streamErrorString(transferred.error);
    Serial.printf("[M4xNet] dl.jsonGet body fail err=%s bytes=%u\n", err,
                  static_cast<unsigned>(body.len));
    // A timed-out/truncated body usually leaves the connection unusable.
    if (h->netTls_) h->netTls_->stop();
    lua_pushnil(L);
    lua_pushstring(L, err);
    return 2;
  }
  Serial.printf("[M4xNet] dl.jsonGet body bytes=%u psram=%d ok\n",
                static_cast<unsigned>(body.len), body.fromCaps ? 1 : 0);
  if (h) h->extendCallbackWallMs(4000);  // parse + table build may exceed the callback budget

  // Keep-alive / idle-EOF can leave a few trailing bytes past the JSON value.
  // Walk the outermost object/array so ArduinoJson does not see InvalidInput.
  auto jsonSpanLen = [](const char* data, size_t len) -> size_t {
    if (!data || len == 0) return 0;
    size_t i = 0;
    while (i < len && static_cast<unsigned char>(data[i]) <= 0x20) ++i;
    if (i >= len) return 0;
    const char open = data[i];
    if (open != '{' && open != '[') return len;
    const char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t j = i; j < len; ++j) {
      const char c = data[j];
      if (inStr) {
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
        } else if (c == '"') {
          inStr = false;
        }
        continue;
      }
      if (c == '"') {
        inStr = true;
        continue;
      }
      if (c == open) {
        ++depth;
      } else if (c == close) {
        --depth;
        if (depth == 0) return j + 1;
      }
    }
    return len;  // incomplete — let the parser report IncompleteInput
  };
  size_t parseLen = jsonSpanLen(body.data, body.len);
  if (parseLen == 0) parseLen = body.len;
  if (parseLen != body.len) {
    Serial.printf("[M4xNet] dl.jsonGet trim body %u -> %u\n", static_cast<unsigned>(body.len),
                  static_cast<unsigned>(parseLen));
  }

  JsonDocument doc(
#if defined(ARDUINO_ARCH_ESP32)
      PsramJsonAllocatorInstance()
#else
      ArduinoJson::detail::DefaultAllocator::instance()
#endif
  );
  DeserializationError derr = deserializeJson(doc, body.data, parseLen);
  if (derr && parseLen != body.len) {
    // Fallback: try full buffer once more.
    derr = deserializeJson(doc, body.data, body.len);
  }
  if (derr) {
    const unsigned head = body.len > 0 ? static_cast<unsigned char>(body.data[0]) : 0;
    const unsigned tail =
        body.len > 0 ? static_cast<unsigned char>(body.data[body.len - 1]) : 0;
    Serial.printf("[M4xNet] dl.jsonGet parse fail: %s head=0x%02x tail=0x%02x len=%u\n",
                  derr.c_str(), head, tail, static_cast<unsigned>(body.len));
    // Diagnosis: dump the raw body (bounded) so a device-side Wi-Fi stream
    // truncation is visible from the host (serial log channel is unreliable).
    if (h) {
      std::string abs;
      std::string rel = "debug_jsonget_fail.bin";
      if (sandboxDataPath(h, rel.c_str(), abs)) {
        FsFile df;
        if (SdMan.openFileForWrite("M4xLua", abs.c_str(), df)) {
          const size_t dumpN = std::min<size_t>(body.len, 4096);
          if (dumpN) df.write(reinterpret_cast<const uint8_t*>(body.data), dumpN);
          char meta[96];
          const int mn = snprintf(meta, sizeof(meta),
                                  "\n--derr=%s len=%u cl=%d\n", derr.c_str(),
                                  static_cast<unsigned>(body.len), cl);
          if (mn > 0) df.write(reinterpret_cast<const uint8_t*>(meta), static_cast<size_t>(mn));
          df.close();
        }
      }
    }
    lua_pushnil(L);
    lua_pushstring(L, "json_parse_failed");
    return 2;
  }

  // Walk `path`, collecting array elements along the way (flatten nested arrays).
  JsonVariant node = doc.as<JsonVariant>();
  for (size_t i = 0; i < path.size() && !node.isNull(); ++i) {
    if (node.is<JsonArray>()) {
      // Array layer: apply the remaining path to every element, collecting
      // elements at the final array layer.
      node = node;  // handled below
      break;
    }
    node = node[path[i].c_str()];
  }
  // Result: an array of arrays (volumes), an array of objects, or a single
  // object (for scalar projections such as chapter content).
  std::vector<JsonVariant> leaves;
  if (node.is<JsonArray>()) {
    for (JsonVariant el : node.as<JsonArray>()) {
      if (el.is<JsonArray>()) {
        for (JsonVariant sub : el.as<JsonArray>()) {
          if (sub.is<JsonObject>()) leaves.push_back(sub);
        }
      } else if (el.is<JsonObject>()) {
        leaves.push_back(el);
      }
    }
  } else if (node.is<JsonObject>()) {
    leaves.push_back(node);
  }

  if (!fileOut.empty()) {
    if (fields.size() != 1 || leaves.size() != 1) {
      lua_pushnil(L);
      lua_pushstring(L, "file_out_requires_one_scalar");
      return 2;
    }
    const char* value = leaves[0][fields[0].c_str()] | "";
    if (!value || !value[0]) {
      lua_pushnil(L);
      lua_pushstring(L, "empty_file_value");
      return 2;
    }
    std::string abs;
    if (M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir().c_str(), fileOut.c_str(), abs) !=
        M4PluginReaderBridge::OpenError::Ok) {
      lua_pushnil(L);
      lua_pushstring(L, "bad_path");
      return 2;
    }
    SdTransactionBackend backend(abs, "DLJ");
    M4xHostIo::TransactionalWriter writer(backend);
    const size_t valueSize = std::strlen(value);
    if (!writer.begin() || !writer.write(reinterpret_cast<const uint8_t*>(value), valueSize) ||
        !writer.commit()) {
      writer.abort();
      lua_pushnil(L);
      lua_pushstring(L, "sd_commit_failed");
      return 2;
    }
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "ok");
    lua_pushnumber(L, static_cast<lua_Number>(valueSize));
    lua_setfield(L, -2, "size");
    Serial.printf("[M4xNet] dl.jsonGet file_out=%s bytes=%u\n", fileOut.c_str(),
                  static_cast<unsigned>(valueSize));
    return 1;
  }

  lua_newtable(L);
  size_t li = 1;
  for (auto& leaf : leaves) {
    lua_newtable(L);
    for (const auto& f : fields) {
      const char* v = leaf[f.c_str()] | "";
      lua_pushstring(L, v);
      lua_setfield(L, -2, f.c_str());
    }
    lua_rawseti(L, -2, static_cast<int>(li++));
  }
  Serial.printf("[M4xNet] dl.jsonGet -> %u items\n", static_cast<unsigned>(leaves.size()));
  return 1;
}

// dl.jsonToFile({url, headers={...}, out="rel", path={...}, fields={...},
//                max_bytes, timeout_ms}) -> {ok=true, count} | {ok=false, error}
// Host-side: stream + SCAN a (possibly multi-hundred-KB) JSON, walk `path`,
// flatten arrays, and write every element's `fields` as one tab-separated line
// to the app-data file. The whole document is never materialized (no ArduinoJson
// pool → safe for large bodies on the device's internal RAM); the result backs
// ui.listOpenFile as an on-SD row source ("virtual memory" for large lists).
int l_dl_jsonToFile(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (const char* denied = M4xHostIo::permissionError(hostIoPermissions(h->app_),
                                                       M4xHostIo::Operation::JsonToFile)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, denied);
    return 2;
  }
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "params_required");
    return 2;
  }
  lua_getfield(L, 1, "url");
  const char* url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  lua_getfield(L, 1, "out");
  const char* rel = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  lua_getfield(L, 1, "max_bytes");
  const size_t maxBytes = lua_isnumber(L, -1) ? static_cast<size_t>(lua_tointeger(L, -1)) : 0;
  lua_pop(L, 1);
  lua_getfield(L, 1, "timeout_ms");
  const uint32_t timeoutMs = lua_isnumber(L, -1) ? static_cast<uint32_t>(lua_tointeger(L, -1)) : 30000;
  lua_pop(L, 1);
  lua_getfield(L, 1, "follow_redirects");
  const bool followRedirects = lua_isboolean(L, -1) && lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "method");
  const char* methodIn = lua_isstring(L, -1) ? lua_tostring(L, -1) : "GET";
  std::string method = methodIn ? methodIn : "GET";
  lua_pop(L, 1);

  for (char& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  std::string requestBody;
  lua_getfield(L, 1, "body");
  if (lua_isstring(L, -1)) {
    size_t bodyLen = 0;
    const char* bodyIn = lua_tolstring(L, -1, &bodyLen);
    if (bodyIn && bodyLen <= 64 * 1024) requestBody.assign(bodyIn, bodyLen);
    else if (bodyLen > 64 * 1024) requestBody.assign("\x01", 1);
  }
  lua_pop(L, 1);
  std::vector<std::pair<std::string, std::string>> headers;
  lua_getfield(L, 1, "headers");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      if (lua_isstring(L, -2) && lua_isstring(L, -1)) headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  if (followRedirects) {
    for (const auto& hv : headers) {
      if (M4xNetPolicy::isSensitiveRequestHeader(hv.first)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "redirect_requires_public_headers");
        return 2;
      }
    }
  }
  std::vector<std::string> path;
  lua_getfield(L, 1, "path");
  if (lua_istable(L, -1)) {
    const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
    for (size_t i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, static_cast<int>(i));
      if (lua_isstring(L, -1)) path.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  std::vector<std::string> fields;
  lua_getfield(L, 1, "fields");
  if (lua_istable(L, -1)) {
    const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
    for (size_t i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, static_cast<int>(i));
      if (lua_isstring(L, -1)) fields.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  if ((method != "GET" && method != "POST") || (method == "POST" && requestBody == "\x01") ||
      !url || !url[0] || !rel || !rel[0] || !M4xHostIo::Limits::validHeaders(headers) ||
      !M4xHostIo::Limits::validJsonShape(path, fields) || !M4xNetPolicy::isAllowedUrl(url)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_params");
    return 2;
  }
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir().c_str(), rel, abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_path");
    return 2;
  }
  const size_t cap = M4xHostIo::Limits::bodyCap(maxBytes, M4xHostIo::Operation::JsonToFile);
  const uint32_t safeTimeout = M4xHostIo::Limits::timeoutMs(timeoutMs);

  auto* secure = new WiFiClientSecure();
  configureTlsClient(secure, h);
  std::unique_ptr<WiFiClient> client(secure);
  HTTPClient http;
  http.setTimeout(static_cast<int>(safeTimeout));
  http.setFollowRedirects(followRedirects ? HTTPC_FORCE_FOLLOW_REDIRECTS
                                          : HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(*client, url)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "http_begin_failed");
    return 2;
  }
  applyHttpHeaders(http, headers, "Mozilla/5.0 (Linux; Android 12) AppleWebKit/537.36 M4xApp/1.0");
  if (h) h->extendCallbackWallMs(safeTimeout + 12000);
  Serial.printf("[M4xNet] dl.jsonToFile begin %s\n", url);
  const int code = method == "POST"
      ? http.POST(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(requestBody.data())),
                  requestBody.size())
      : http.GET();
  if (code < 0 || (code >= 300 && code < 400)) {
    http.end();
    lua_pushboolean(L, 0);
    lua_pushstring(L, code < 0 ? "http_request_failed" : "redirect_unsupported");
    return 2;
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    lua_pushboolean(L, 0);
    lua_pushstring(L, ("http_" + std::to_string(code)).c_str());
    return 2;
  }
  const int cl = http.getSize();
  if (cl > 0 && static_cast<size_t>(cl) > cap) {
    http.end();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "response_too_large");
    return 2;
  }
  Serial.printf("[M4xNet] dl.jsonToFile %s %s cap=%u body=%u\n", method.c_str(), url,
                static_cast<unsigned>(cap), static_cast<unsigned>(requestBody.size()));
  SdTransactionBackend backend(abs, "DLF");
  M4xHostIo::TransactionalWriter writer(backend);
  if (!writer.begin()) {
    http.end();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "sd_open_failed");
    return 2;
  }
  TransactionJsonSink sink(writer);
  M4xJsonStream::RecordExtractor extractor(path, fields, sink);
  JsonExtractorStreamSink streamSink(extractor);
  const String teHeader = http.header("Transfer-Encoding");
  const bool chunked = teHeader.length() > 0 && teHeader.indexOf("chunked") >= 0;
  NetworkBodySource source(http.getStreamPtr(), chunked);
  const size_t expected = cl >= 0 ? static_cast<size_t>(cl) : static_cast<size_t>(-1);
  const M4xHostIo::StreamResult transferred =
      M4xHostIo::stream(source, streamSink, cap, expected, safeTimeout, hostStreamRuntime());
  const size_t got = transferred.bytes;
  http.end();
  bool fail = !transferred.ok();
  std::string err = fail ? M4xHostIo::streamErrorString(transferred.error) : "";
  if (transferred.error == M4xHostIo::StreamError::Sink && extractor.error() != M4xJsonStream::Error::None) {
    err = M4xJsonStream::errorString(extractor.error());
  }
  if (!fail && !extractor.finish()) {
    err = M4xJsonStream::errorString(extractor.error());
    fail = true;
  }
  if (!fail && !sink.flush()) {
    err = "sd_write_failed";
    fail = true;
  }
  if (!fail && !writer.commit()) {
    err = "sd_commit_failed";
    fail = true;
  }
  if (fail) {
    writer.abort();
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  const size_t count = extractor.recordCount();
  lua_newtable(L);
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "ok");
  lua_pushnumber(L, static_cast<lua_Number>(count));
  lua_setfield(L, -2, "count");
  Serial.printf("[M4xNet] dl.jsonToFile -> %u rows, %u bytes, peak=%u\n",
                static_cast<unsigned>(count), static_cast<unsigned>(got),
                static_cast<unsigned>(extractor.peakBufferedBytes()));
  return 1;
}

// ---- loader: progressive chapter/toc (URL + extract → early native handoff) ----

static void readHeaderMap(lua_State* L, int idx, std::vector<std::pair<std::string, std::string>>& out) {
  lua_getfield(L, idx, "headers");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
        out.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

static void readStringPath(lua_State* L, int idx, const char* key, std::vector<std::string>& out) {
  lua_getfield(L, idx, key);
  if (lua_istable(L, -1)) {
    const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
    for (size_t i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, static_cast<int>(i));
      if (lua_isstring(L, -1)) out.emplace_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

int l_loader_chapter(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "params_required");
    return 2;
  }
  M4xProgressiveLoader::ChapterSpec spec;
  lua_getfield(L, 1, "url");
  spec.url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  lua_getfield(L, 1, "out");
  spec.relOut = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  readHeaderMap(L, 1, spec.headers);
  lua_getfield(L, 1, "extract");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "kind");
    const char* kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : "json_field";
    lua_pop(L, 1);
    if (kind && std::strcmp(kind, "raw") == 0) {
      spec.rawBody = true;
    } else {
      readStringPath(L, -1, "path", spec.jsonPath);  // relative to extract table: wrong index
    }
    lua_getfield(L, -1, "field");
    if (lua_isstring(L, -1)) spec.field = lua_tostring(L, -1);
    lua_pop(L, 1);
    // path inside extract
    lua_getfield(L, -1, "path");
    if (lua_istable(L, -1)) {
      spec.jsonPath.clear();
      const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
      for (size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i));
        if (lua_isstring(L, -1)) spec.jsonPath.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  } else {
    // Shorthand: field at top level
    lua_getfield(L, 1, "field");
    if (lua_isstring(L, -1)) spec.field = lua_tostring(L, -1);
    lua_pop(L, 1);
    readStringPath(L, 1, "path", spec.jsonPath);
    if (spec.field.empty()) spec.field = "content";
  }
  lua_pop(L, 1);
  if (spec.field.empty() && !spec.rawBody) spec.field = "content";

  lua_getfield(L, 1, "early_bytes");
  if (lua_isnumber(L, -1)) spec.earlyBytes = static_cast<size_t>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "max_bytes");
  if (lua_isnumber(L, -1)) spec.maxBytes = static_cast<size_t>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "timeout_ms");
  if (lua_isnumber(L, -1)) spec.timeoutMs = static_cast<uint32_t>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "follow_redirects");
  if (lua_isboolean(L, -1)) spec.followRedirects = lua_toboolean(L, -1);
  lua_pop(L, 1);

  auto getStr = [&](const char* k) -> const char* {
    lua_getfield(L, 1, k);
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s ? s : "";
  };
  spec.open.title = getStr("title");
  spec.open.bookId = getStr("bookId");
  spec.open.chapterUid = getStr("chapterUid");
  spec.open.progressKey = getStr("progressKey");
  spec.open.providerId = getStr("providerId");
  spec.open.appId = h->app_.id;
  lua_getfield(L, 1, "chapterIndex");
  if (lua_isnumber(L, -1)) spec.open.chapterIndex = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "initialByteOffset");
  if (lua_isnumber(L, -1)) {
    spec.open.initialByteOffset = static_cast<uint64_t>(lua_tointeger(L, -1));
    spec.open.hasInitialByteOffset = true;
  }
  lua_pop(L, 1);
  spec.open.generation = M4PluginReaderSession::bumpGeneration();
  spec.open.relPath = spec.relOut;

  if (spec.url.empty() || spec.relOut.empty() || !M4xNetPolicy::isAllowedUrl(spec.url.c_str())) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_params");
    return 2;
  }
  if (M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir().c_str(), spec.relOut.c_str(), spec.absOut) !=
      M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_path");
    return 2;
  }
  h->extendCallbackWallMs(spec.timeoutMs + 5000);
  std::string err;
  if (!M4xProgressiveLoader::session().beginChapter(std::move(spec), err)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  // Do NOT prime many pumps here: connect + body used to freeze the e-ink
  // loading screen for seconds with no progress. Host/plugin pump between
  // draws so status bytes/phase can refresh. Tiny chapters still open on the
  // next few Tick/draw pumps (earlyBytes default ~1.5KB).
  lua_pushboolean(L, 1);
  return 1;
}

int l_loader_toc(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "params_required");
    return 2;
  }
  M4xProgressiveLoader::TocSpec spec;
  lua_getfield(L, 1, "url");
  spec.url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  lua_getfield(L, 1, "out");
  spec.relOut = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  readHeaderMap(L, 1, spec.headers);
  readStringPath(L, 1, "path", spec.jsonPath);
  lua_getfield(L, 1, "fields");
  if (lua_istable(L, -1)) {
    const size_t n = static_cast<size_t>(lua_rawlen(L, -1));
    for (size_t i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, static_cast<int>(i));
      if (lua_isstring(L, -1)) spec.fields.emplace_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  if (spec.fields.empty()) {
    spec.fields = {"chapterid", "chaptername", "chaptertype", "isvip", "islock"};
  }
  if (spec.jsonPath.empty()) spec.jsonPath = {"chapterlist"};

  lua_getfield(L, 1, "early_rows");
  if (lua_isnumber(L, -1)) spec.earlyRows = static_cast<size_t>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "max_bytes");
  if (lua_isnumber(L, -1)) spec.maxBytes = static_cast<size_t>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "timeout_ms");
  if (lua_isnumber(L, -1)) spec.timeoutMs = static_cast<uint32_t>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "follow_redirects");
  if (lua_isboolean(L, -1)) spec.followRedirects = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 1, "uidField");
  if (lua_isnumber(L, -1)) spec.uidField0 = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "titleField");
  if (lua_isnumber(L, -1)) spec.titleField0 = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, 1, "vipField");
  if (lua_isnumber(L, -1)) spec.vipField0 = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);

  auto getStr = [&](const char* k) -> const char* {
    lua_getfield(L, 1, k);
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s ? s : "";
  };
  spec.open.bookId = getStr("bookId");
  spec.open.bookTitle = getStr("title");
  spec.open.providerId = getStr("providerId");
  spec.open.appId = h->app_.id;
  spec.open.appDataRoot = h->dataDir();
  lua_getfield(L, 1, "currentIndex");
  if (lua_isnumber(L, -1)) spec.open.currentIndex = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  spec.open.generation = M4PluginReaderSession::bumpGeneration();

  if (spec.url.empty() || spec.relOut.empty() || !M4xNetPolicy::isAllowedUrl(spec.url.c_str())) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_params");
    return 2;
  }
  if (M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir().c_str(), spec.relOut.c_str(), spec.absOut) !=
      M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_path");
    return 2;
  }
  h->extendCallbackWallMs(spec.timeoutMs + 5000);
  std::string err;
  if (!M4xProgressiveLoader::session().beginToc(std::move(spec), err)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  // No multi-pump prime (same reason as chapter): keep loading UI responsive.
  lua_pushboolean(L, 1);
  return 1;
}

int l_loader_pump(lua_State* L) {
  uint32_t budgetMs = 100;
  size_t budgetBytes = 16 * 1024;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "ms");
    if (lua_isnumber(L, -1)) budgetMs = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "bytes");
    if (lua_isnumber(L, -1)) budgetBytes = static_cast<size_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
  }
  auto* h = hostFromLua(L);
  // Connecting (TLS) can take several seconds; body slices stay short.
  if (h) h->extendCallbackWallMs(budgetMs + 8000);
  const bool more = M4xProgressiveLoader::session().pump(budgetMs, budgetBytes);
  lua_pushboolean(L, more ? 1 : 0);
  return 1;
}

int l_loader_status(lua_State* L) {
  const auto st = M4xProgressiveLoader::session().status();
  lua_newtable(L);
  lua_pushboolean(L, st.active ? 1 : 0);
  lua_setfield(L, -2, "active");
  lua_pushstring(L, st.kind == M4xProgressiveLoader::Kind::Chapter
                        ? "chapter"
                        : (st.kind == M4xProgressiveLoader::Kind::Toc ? "toc" : "none"));
  lua_setfield(L, -2, "kind");
  lua_pushstring(L, st.phaseName ? st.phaseName : "idle");
  lua_setfield(L, -2, "phase");
  lua_pushnumber(L, static_cast<lua_Number>(st.bytes));
  lua_setfield(L, -2, "bytes");
  lua_pushnumber(L, static_cast<lua_Number>(st.rows));
  lua_setfield(L, -2, "rows");
  lua_pushboolean(L, st.early ? 1 : 0);
  lua_setfield(L, -2, "early");
  lua_pushboolean(L, st.done ? 1 : 0);
  lua_setfield(L, -2, "done");
  lua_pushstring(L, st.error ? st.error : "");
  lua_setfield(L, -2, "error");
  lua_pushstring(L, st.path ? st.path : "");
  lua_setfield(L, -2, "path");
  return 1;
}

int l_loader_cancel(lua_State* L) {
  (void)L;
  M4xProgressiveLoader::session().cancel();
  return 0;
}


int l_dl_download(lua_State* L) {
  auto* h = hostFromLua(L);
  if (!h) return luaL_error(L, "no host");
  if (const char* denied = M4xHostIo::permissionError(hostIoPermissions(h->app_),
                                                       M4xHostIo::Operation::Download)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, denied);
    return 2;
  }
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "params_required");
    return 2;
  }
  lua_getfield(L, 1, "url");
  const char* url = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  lua_getfield(L, 1, "path");
  const char* rel = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  lua_getfield(L, 1, "max_bytes");
  const size_t maxBytes = lua_isnumber(L, -1) ? static_cast<size_t>(lua_tointeger(L, -1)) : 0;
  lua_pop(L, 1);
  lua_getfield(L, 1, "timeout_ms");
  const uint32_t timeoutMs = lua_isnumber(L, -1) ? static_cast<uint32_t>(lua_tointeger(L, -1)) : 30000;
  lua_pop(L, 1);

  std::vector<std::pair<std::string, std::string>> headers;
  lua_getfield(L, 1, "headers");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
        headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  if (!url || !url[0] || !rel || !rel[0] || !M4xHostIo::Limits::validHeaders(headers) ||
      !M4xNetPolicy::isAllowedUrl(url)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "https_required");
    return 2;
  }
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(h->dataDir().c_str(), rel, abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "bad_path");
    return 2;
  }
  const size_t cap = M4xHostIo::Limits::bodyCap(maxBytes, M4xHostIo::Operation::Download);
  const uint32_t safeTimeout = M4xHostIo::Limits::timeoutMs(timeoutMs);
  size_t outSize = 0;
  char shaHex[65] = {0};
  std::string err;
  const bool ok = dlStreamToFile(url, headers, abs, cap, safeTimeout, outSize, shaHex, err);
  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) {
    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(outSize));
    lua_setfield(L, -2, "size");
    lua_pushstring(L, shaHex);
    lua_setfield(L, -2, "sha256");
    return 2;
  }
  lua_pushstring(L, err.c_str());
  return 2;
}

void registerModule(lua_State* L, const char* name, const luaL_Reg* regs) {
  lua_newtable(L);
  luaL_setfuncs(L, regs, 0);
  lua_setglobal(L, name);
}

void openSafeLibs(lua_State* L) {
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
  lua_pop(L, 1);
}

bool callGlobal(lua_State* L, M4xLuaSandbox::Budget* budget, const char* fn, int narg, uint32_t wallMs,
                std::string& errorOut) {
  if (lua_getglobal(L, fn) != LUA_TFUNCTION) {
    lua_pop(L, 1 + narg);
    return true;  // missing optional hook is OK
  }
  if (narg > 0) lua_insert(L, -(narg + 1));
  M4xLuaSandbox::beginCallback(budget, wallMs);
  if (lua_pcall(L, narg, 0, 0) != LUA_OK) {
    if (budget && budget->reason) {
      errorOut = budget->reason;
      if (lua_tostring(L, -1)) {
        errorOut += ": ";
        errorOut += lua_tostring(L, -1);
      }
    } else {
      errorOut = lua_tostring(L, -1) ? lua_tostring(L, -1) : "lua_error";
    }
    lua_pop(L, 1);
    // Developer mode: persist Lua callback failures to SD (same gate as log()).
    if (gHost && SETTINGS.developerSerialDebugEnabled != 0) {
      std::string line = "lua:";
      line += fn ? fn : "?";
      line += " ";
      line += errorOut;
      appendPluginErrorLog(gHost, line.c_str());
    }
    return false;
  }
  return true;
}

}  // namespace

void M4xLuaHost::closeUiScene() {
  if (!uiScene_.active) return;
  uiScene_.active = false;
  uiScene_.repaint = true;  // force the next normal Lua paint
  uiScene_.generation++;
}

bool M4xLuaHost::renderUiScene() {
  if (!uiScene_.active || !renderer_) return false;
  auto& sc = uiScene_;
  auto& r = *renderer_;
  const M4UiStyle::Theme style = uiSceneStyle(*this);
  const size_t rowCount = uiSceneRowCount(sc);
  const int pageSize = M4xUiList::effectivePageSize(sc.pageSize, style.list.visibleRows);
  const int total = sc.pageCountOverride > 0
      ? sc.pageCountOverride
      : M4xUiList::totalPages(rowCount, sc.pageSize, style.list.visibleRows);

  r.clearScreen();
  const int w = r.getScreenWidth();
  const int left = style.metrics.content.x;
  // Use the active system header (battery, truncation and divider) instead
  // of letting each plugin invent its own title bar.
  const ThemeMetrics& systemMetrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(r, Rect{0, systemMetrics.topPadding, w, systemMetrics.headerHeight}, sc.title.c_str());
  // Footer hint line.
  if (!sc.footer.empty()) {
    // Keep the hint in the gutter between the native header divider and the
    // first list row.  Drawing it only 12px above the content origin makes a
    // reader-font glyph touch the first row's ascender; the active system
    // spacing is 16px, so reserve a small four-pixel breathing room as well.
    const int statusGap = std::max(16, systemMetrics.verticalSpacing + 4);
    const int statusY = std::max(0, style.metrics.header.height - statusGap);
    M4UiText::draw(r, UI_10_FONT_ID, left, statusY,
                   M4UiText::truncated(r, UI_10_FONT_ID, sc.footer.c_str(), w - 2 * left).c_str());
  }
  // Page buttons use the same footer rectangles used by touch hit testing;
  // no per-scene 480x800 coordinates or tiny untappable glyph boxes.
  const M4UiStyle::Rect leftButton = M4UiStyle::footerButtonRect(style, 0, 2);
  const M4UiStyle::Rect rightButton = M4UiStyle::footerButtonRect(style, 1, 2);
  r.drawRect(leftButton.x, leftButton.y, leftButton.width, leftButton.height, true);
  r.drawRect(rightButton.x, rightButton.y, rightButton.width, rightButton.height, true);
  M4UiText::drawCenteredInBox(r, UI_10_FONT_ID, leftButton.x, leftButton.y, leftButton.width,
                              leftButton.height, "‹ 上一页", true);
  M4UiText::drawCenteredInBox(r, UI_10_FONT_ID, rightButton.x, rightButton.y, rightButton.width,
                              rightButton.height, "下一页 ›", true);
  // Page indicator centered in the dedicated middle slot; it must never sit
  // on top of either button border or label.
  char pageStr[48];
  snprintf(pageStr, sizeof(pageStr), "%d/%d", sc.page, total);
  const M4UiStyle::Rect pageSlot = M4UiStyle::footerPageSlot(style, 2);
  M4UiText::drawCenteredInBox(r, UI_10_FONT_ID, pageSlot.x, pageSlot.y, pageSlot.width, pageSlot.height,
                              pageStr, true, EpdFontFamily::REGULAR, 2);

  // Give the current page to the system list component.  This preserves a
  // plugin's requested page size without allowing GUI.drawList()'s own page
  // calculation to select a different slice of the catalog.
  const size_t start = sc.remotePage
      ? 0
      : M4xUiList::rowStart(sc.page, sc.pageSize, style.list.visibleRows);
  const size_t n = sc.remotePage
      ? std::min(rowCount, static_cast<size_t>(pageSize))
      : M4xUiList::visibleCount(rowCount, sc.page, sc.pageSize, style.list.visibleRows);
  const M4FileRows::PageResult filePage = sc.fromFile ? uiSceneFilePage(sc, sc.page) : M4FileRows::PageResult{};
  const int pageCount = std::min(pageSize, static_cast<int>(n));
  GUI.drawList(
      r, Rect{0, style.metrics.content.y, w, style.metrics.content.height}, pageCount, -1,
      [&](int localIndex) {
        const size_t i = static_cast<size_t>(localIndex);
        if (i >= n) return std::string{};
        if (sc.fromFile) {
          if (!filePage || i >= filePage.rows.size()) return std::string{};
          const std::string& line = filePage.rows[i].line;
          const size_t tab = line.find('\t');
          return tab == std::string::npos ? line : line.substr(tab + 1);
        }
        return sc.rows[start + i];
      },
      sc.hasSubtitles
          ? std::function<std::string(int)>([&](int localIndex) {
              const size_t i = static_cast<size_t>(localIndex);
              if (i >= n || sc.fromFile) return std::string{};
              return sc.rowSubs[start + i];
            })
          : nullptr,
      nullptr, nullptr);
  return true;
}

bool M4xLuaHost::uiCallGlobal(const char* fn, std::string& errorOut, int nargs) {
  if (!L_ || !fn || !fn[0]) return true;
  auto* L = static_cast<lua_State*>(L_);
  lua_getglobal(L, fn);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1 + nargs);
    return true;
  }
  lua_insert(L, -(nargs + 1));
  return callGlobal(L, &budget_, fn, nargs, M4xLuaSandbox::kDefaultWallMs, errorOut);
}

bool M4xLuaHost::uiCallRow(int index0, std::string& errorOut) {
  auto* L = static_cast<lua_State*>(L_);
  if (uiScene_.fromFile) {
    const size_t totalRows = uiSceneRowCount(uiScene_);
    if (index0 < 0 || static_cast<size_t>(index0) >= totalRows || !uiScene_.fileSource) return true;
    const int visibleRows = renderer_ ? uiSceneStyle(*this).list.visibleRows : 12;
    const int sourcePageSize = M4xUiList::effectivePageSize(uiScene_.pageSize, visibleRows);
    const int page = static_cast<int>(static_cast<size_t>(index0) / static_cast<size_t>(sourcePageSize)) + 1;
    const M4FileRows::PageResult pageRows = uiSceneFilePage(uiScene_, page);
    const size_t local = static_cast<size_t>(index0) % static_cast<size_t>(sourcePageSize);
    if (!pageRows || local >= pageRows.rows.size()) return true;
    const std::string& line = pageRows.rows[local].line;
    lua_pushnumber(L, index0);
    size_t start = 0;
    int pushed = 1;
    for (size_t i = 0; i <= line.size() && pushed < 17; ++i) {
      if (i == line.size() || line[i] == '\t') {
        lua_pushlstring(L, line.data() + start, i - start);
        start = i + 1;
        ++pushed;
      }
    }
    return uiCallGlobal(uiScene_.onRow.c_str(), errorOut, pushed);
  }
  if (index0 < 0) return true;
  const int localIndex = uiScene_.remotePage
      ? index0 - static_cast<int>(M4xUiList::rowStart(uiScene_.page, uiScene_.pageSize,
                                                      renderer_ ? uiSceneStyle(*this).list.visibleRows : 12))
      : index0;
  if (localIndex < 0 || localIndex >= static_cast<int>(uiScene_.rows.size())) return true;
  lua_pushnumber(L, uiScene_.remotePage ? index0 : localIndex);
  lua_pushstring(L, uiScene_.rows[static_cast<size_t>(localIndex)].c_str());
  lua_pushstring(L, uiScene_.rowSubs[static_cast<size_t>(localIndex)].c_str());
  return uiCallGlobal(uiScene_.onRow.c_str(), errorOut, 3);
}

bool M4xLuaHost::uiCallPage(int newPage, std::string& errorOut) {
  const size_t rowCount = uiSceneRowCount(uiScene_);
  const M4UiStyle::Theme style = uiSceneStyle(*this);
  const int total = uiScene_.pageCountOverride > 0
      ? uiScene_.pageCountOverride
      : M4xUiList::totalPages(rowCount, uiScene_.pageSize, style.list.visibleRows);
  newPage = M4xUiList::clampPage(newPage, total);
  if (newPage == uiScene_.page) return true;
  uiScene_.page = newPage;
  uiScene_.repaint = true;
  auto* L = static_cast<lua_State*>(L_);
  lua_pushnumber(L, newPage);
  lua_pushnumber(L, total);
  return uiCallGlobal(uiScene_.onPage.c_str(), errorOut, 2);
}

bool M4xLuaHost::uiCallBack(std::string& errorOut) {
  auto* L = static_cast<lua_State*>(L_);
  (void)L;
  return uiCallGlobal(uiScene_.onBack.c_str(), errorOut, 0);
}

bool M4xLuaHost::handleUiSceneKey(const char* key, std::string& errorOut) {
  if (!uiScene_.active || !key) return false;
  const std::string k(key);
  if (k == "back") {
    return uiCallBack(errorOut);
  }
  if (k == "left") {
    return uiCallPage(uiScene_.page - 1, errorOut);
  }
  if (k == "right" || k == "confirm" || k == "center") {
    return uiCallPage(uiScene_.page + 1, errorOut);
  }
  return false;
}

bool M4xLuaHost::handleUiSceneTouch(int x, int y, const char* phase, std::string& errorOut) {
  if (!uiScene_.active || !phase) return false;
  if (std::strcmp(phase, "tap") != 0 && std::strcmp(phase, "up") != 0 &&
      std::strcmp(phase, "press") != 0) {
    return true;  // consume other phases while scene owns input
  }
  const M4UiStyle::Theme style = uiSceneStyle(*this);
  if (y >= 0 && y < style.metrics.content.y) {
    return uiCallBack(errorOut);
  }
  if (y >= style.metrics.footer.y) {
    const int button = M4UiStyle::footerButtonAt(style, x, y, 2);
    if (button < 0) return true;
    return uiCallPage(uiScene_.page + (button == 0 ? -1 : 1), errorOut);
  }
  const size_t rowCount = uiSceneRowCount(uiScene_);
  const int pageSize = M4xUiList::effectivePageSize(uiScene_.pageSize, style.list.visibleRows);
  const size_t visible = uiScene_.remotePage
      ? std::min(rowCount, static_cast<size_t>(M4xUiList::effectivePageSize(
          uiScene_.pageSize, style.list.visibleRows)))
      : M4xUiList::visibleCount(rowCount, uiScene_.page, uiScene_.pageSize,
                                style.list.visibleRows);
  const int row = M4UiStyle::rowAt(style, x, y, static_cast<int>(visible));
  if (row < 0) return true;
  // uiCallRow expects a global zero-based index.  Remote pages render their
  // resident rows from local offset zero, but touch callbacks still need the
  // logical page offset so providers can map the row deterministically.
  const size_t start = M4xUiList::rowStart(uiScene_.page, uiScene_.pageSize,
                                           style.list.visibleRows);
  (void)pageSize;
  return uiCallRow(static_cast<int>(start) + row, errorOut);
}


M4xLuaHost::M4xLuaHost() = default;
M4xLuaHost::~M4xLuaHost() { stop(); }

void M4xLuaHost::releaseNetworkSession() {
  // The keep-alive mbedTLS session retains ~32KB of internal RAM (in/out
  // content buffers). Drop it before the native reader runs (no networking
  // needed there) so the next chapter open keeps internal headroom.
  if (netHttp_) netHttp_->end();
  if (netTls_) netTls_->stop();
  netHttp_.reset();
  netTls_.reset();
}

bool M4xLuaHost::start(GfxRenderer& renderer, const M4xInstalledApp& app, std::string& errorOut) {
  stop();
  renderer_ = &renderer;
  app_ = app;
  exitRequested_ = false;
  wifiOwned_ = false;
  dataDir_ = std::string(M4xPaths::kAppsDataRoot) + "/" + app.id;
  installDir_ = app.path;
  SdMan.mkdir(dataDir_.c_str(), true);

  clearCancel();
  budget_ = M4xLuaSandbox::Budget{};
  budget_.memLimit = psramFound() ? M4xLuaSandbox::kPsramHeapLimit
                                  : M4xLuaSandbox::kDefaultHeapLimit;
  budget_.instrBudget = M4xLuaSandbox::kDefaultInstrBudget;
  budget_.nowMs = &hostNowMs;
  budget_.cancelFlag = &cancelRequested_;

  lua_State* L = lua_newstate(M4xLuaSandbox::alloc, &budget_);
  if (!L) {
    errorOut = "lua_newstate_failed";
    return false;
  }
  L_ = L;
  gHost = this;
  M4xLuaSandbox::installHook(L, &budget_);
  openSafeLibs(L);

  // Developer mode: mark log path so USB sd_read can confirm logging is armed.
  if (SETTINGS.developerSerialDebugEnabled != 0) {
    char boot[160];
    snprintf(boot, sizeof(boot), "host_start app=%s log=apps_data/%s/logs/error.log", app.id.c_str(),
             app.id.c_str());
    appendPluginErrorLog(this, boot);
    Serial.printf("[M4xLog] developer log armed path=%s/logs/error.log\n", dataDir_.c_str());
  }

  lua_register(L, "log", l_log);
  lua_register(L, "logError", l_logError);

  static const luaL_Reg guiRegs[] = {
      {"width", l_gui_width},
      {"height", l_gui_height},
      {"clear", l_gui_clear},
      {"drawText", l_gui_drawText},
      {"textWidth", l_gui_textWidth},
      {"lineHeight", l_gui_lineHeight},
      {"drawRect", l_gui_drawRect},
      {"fillRect", l_gui_fillRect},
      {"drawLine", l_gui_drawLine},
      {"drawQR", l_gui_drawQR},
      {"qrSize", l_gui_qrSize},
      {"refresh", l_gui_refresh},
      {nullptr, nullptr},
  };
  registerModule(L, "gui", guiRegs);

  static const luaL_Reg sysRegs[] = {
      {"millis", l_sys_millis},
      {"time", l_sys_time},
      {"delay", l_sys_delay},
      {"exit", l_sys_exit},
      {"memInfo", l_sys_memInfo},
      {"load", l_sys_load},
      {"fontInfo", l_sys_fontInfo},
      {"developerMode", l_sys_developerMode},
      {nullptr, nullptr},
  };
  registerModule(L, "sys", sysRegs);

  static const luaL_Reg fsRegs[] = {
      {"readFile", l_fs_readFile},
      {"writeFile", l_fs_writeFile},
      {"replaceFile", l_fs_replaceFile},
      {"fileSize", l_fs_fileSize},
      {"readRange", l_fs_readRange},
      {"readCatalogRow", l_fs_readCatalogRow},
      {"readAppFile", l_fs_readAppFile},
      {nullptr, nullptr},
  };
  registerModule(L, "fs", fsRegs);

  static const luaL_Reg netRegs[] = {
      {"isConnected", l_net_isConnected},
      {"connectSaved", l_net_connectSaved},
      {"request", l_net_request},
      {"extractPsvts", l_net_extractPsvts},
      {nullptr, nullptr},
  };
  registerModule(L, "net", netRegs);

  static const luaL_Reg jsonRegs[] = {
      {"decode", l_json_decode},
      {"encode", l_json_encode},
      {nullptr, nullptr},
  };
  registerModule(L, "json", jsonRegs);

  static const luaL_Reg cryptoRegs[] = {
      {"md5", l_crypto_md5},
      {"urlEncode", l_crypto_urlEncode},
      {nullptr, nullptr},
  };
  registerModule(L, "crypto", cryptoRegs);

  static const luaL_Reg wereadRegs[] = {
      {"e", l_weread_e},
      {"sign", l_weread_sign},
      {"sortedQuery", l_weread_sortedQuery},
      {"makeContentParams", l_weread_makeContentParams},
      {"decodeShards", l_weread_decodeShards},
      {"extractPsvts", l_weread_extractPsvts},
      {"stripXhtml", l_weread_stripXhtml},
      {nullptr, nullptr},
  };
  registerModule(L, "weread", wereadRegs);

  static const luaL_Reg readerRegs[] = {
      {"openText", l_reader_openText},
      {"openToc", l_reader_openToc},
      {nullptr, nullptr},
  };
  registerModule(L, "reader", readerRegs);

  static const luaL_Reg providerRegs[] = {
      {"register", l_provider_register},
      {"setChapter", l_provider_setChapter},
      {"pollWork", l_provider_pollWork},
      {"resolveCatalogWork", l_provider_resolveCatalogWork},
      {"historyUri", l_provider_historyUri},
      {"takeResume", l_provider_takeResume},
      {nullptr, nullptr},
  };
  registerModule(L, "provider", providerRegs);

  static const luaL_Reg uiRegs[] = {
      {"listOpen", l_ui_listOpen},
      {"listOpenFile", l_ui_listOpenFile},
      {"listSetRows", l_ui_listSetRows},
      {"listAppendRows", l_ui_listAppendRows},
      {"listPrependRows", l_ui_listPrependRows},
      {"listClose", l_ui_listClose},
      {"listPage", l_ui_listPage},
      {nullptr, nullptr},
  };
  registerModule(L, "ui", uiRegs);

  static const luaL_Reg dlRegs[] = {
      {"download", l_dl_download},
      {"jsonGet", l_dl_jsonGet},
      {"jsonToFile", l_dl_jsonToFile},
      {nullptr, nullptr},
  };
  registerModule(L, "dl", dlRegs);

  static const luaL_Reg loaderRegs[] = {
      {"chapter", l_loader_chapter},
      {"toc", l_loader_toc},
      {"pump", l_loader_pump},
      {"status", l_loader_status},
      {"cancel", l_loader_cancel},
      {nullptr, nullptr},
  };
  registerModule(L, "loader", loaderRegs);

  lua_pushnumber(L, 0);
  lua_setglobal(L, "COLOR_WHITE");
  lua_pushnumber(L, 1);
  lua_setglobal(L, "COLOR_BLACK");

  std::string scriptPath = installDir_;
  if (!scriptPath.empty() && scriptPath.back() != '/') scriptPath += '/';
  scriptPath += app.entry.empty() ? "main.lua" : app.entry;

  FsFile f;
  if (!SdMan.openFileForRead("M4xLua", scriptPath.c_str(), f)) {
    errorOut = std::string("open_entry:") + scriptPath;
    stop();
    return false;
  }
  const size_t n = f.fileSize();
  if (n > M4xPathSafe::kMaxEntryBytes) {
    f.close();
    errorOut = "entry_too_large";
    stop();
    return false;
  }
  std::string src;
  src.resize(n);
  if (n) {
    FsFileCtx ctx{&f};
    if (!M4xLuaSandbox::readExact(fsReadChunk, &ctx, reinterpret_cast<uint8_t*>(&src[0]), n)) {
      f.close();
      errorOut = "entry_short_read";
      stop();
      return false;
    }
  }
  f.close();

  M4xLuaSandbox::beginCallback(&budget_, M4xLuaSandbox::kStartWallMs);
  if (luaL_loadbuffer(L, src.data(), src.size(), app.id.c_str()) != LUA_OK) {
    errorOut = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load_error";
    stop();
    return false;
  }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    if (budget_.reason) {
      errorOut = budget_.reason;
      if (lua_tostring(L, -1)) {
        errorOut += ": ";
        errorOut += lua_tostring(L, -1);
      }
    } else {
      errorOut = lua_tostring(L, -1) ? lua_tostring(L, -1) : "run_error";
    }
    stop();
    return false;
  }

  if (!callGlobal(L, &budget_, "init", 0, M4xLuaSandbox::kStartWallMs, errorOut)) {
    stop();
    return false;
  }
  return true;
}

bool M4xLuaHost::callDraw(std::string& errorOut) {
  if (!L_) return false;
  auto* L = static_cast<lua_State*>(L_);
  // Fanqie/WeRead use a two-phase loading draw: the first call paints the
  // loading page, and a later draw performs one bounded HTTP/SD hop.  That
  // second call is still a Lua callback, so the ordinary 8 s UI budget used
  // to kill valid 30 s network requests with "callback time budget exceeded".
  uint32_t wallMs = M4xLuaSandbox::kDefaultWallMs;
  lua_getglobal(L, "screen");
  const bool loading = lua_isstring(L, -1) && std::strcmp(lua_tostring(L, -1), "loading") == 0;
  lua_pop(L, 1);
  if (loading) wallMs = M4xLuaSandbox::kNetworkWallMs;
  return callGlobal(L, &budget_, "draw", 0, wallMs, errorOut);
}

bool M4xLuaHost::wantsPump() const {
  if (!L_) return false;
  auto* L = static_cast<lua_State*>(L_);
  lua_getglobal(L, "dirty");
  bool pump = false;
  if (lua_isboolean(L, -1)) {
    pump = lua_toboolean(L, -1) != 0;
  } else if (lua_isnumber(L, -1)) {
    pump = lua_tonumber(L, -1) != 0;
  }
  lua_pop(L, 1);
  return pump;
}

bool M4xLuaHost::frameChanged() const {
  if (!L_) return true;
  auto* L = static_cast<lua_State*>(L_);
  lua_getglobal(L, "frame_changed");
  // Missing or non-boolean ⇒ display (preserve existing plugins).
  const bool changed = !lua_isboolean(L, -1) || lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return changed;
}

bool M4xLuaHost::callOnKey(const char* keyName, std::string& errorOut) {
  if (!L_ || !keyName) return true;
  auto* L = static_cast<lua_State*>(L_);
  lua_pushstring(L, keyName);
  return callGlobal(L, &budget_, "onKey", 1, M4xLuaSandbox::kDefaultWallMs, errorOut);
}

bool M4xLuaHost::callOnTouch(int x, int y, const char* phase, std::string& errorOut) {
  if (!L_) return true;
  auto* L = static_cast<lua_State*>(L_);
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  lua_pushstring(L, phase ? phase : "tap");
  return callGlobal(L, &budget_, "onTouch", 3, M4xLuaSandbox::kDefaultWallMs, errorOut);
}

bool M4xLuaHost::callOnReaderClosed(int page, int total, size_t byteOffset, bool complete, const char* bookId,
                                    const char* chapterUid, const char* progressKey, std::string& errorOut,
                                    const char* openError, int switchChapterIndex) {
  if (!L_) return true;
  auto* L = static_cast<lua_State*>(L_);
  lua_newtable(L);
  // Convert exactly once: native PluginProgress.page is 0-based → Lua 1-based.
  lua_pushinteger(L, M4PluginReaderStatePolicy::page0ToLua1(page));
  lua_setfield(L, -2, "page");
  if (total >= 0) {
    lua_pushinteger(L, total);
    lua_setfield(L, -2, "total");
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "total");
  }
  lua_pushinteger(L, static_cast<lua_Integer>(byteOffset));
  lua_setfield(L, -2, "byteOffset");
  lua_pushboolean(L, complete ? 1 : 0);
  lua_setfield(L, -2, "complete");
  lua_pushstring(L, bookId ? bookId : "");
  lua_setfield(L, -2, "bookId");
  lua_pushstring(L, chapterUid ? chapterUid : "");
  lua_setfield(L, -2, "chapterUid");
  lua_pushstring(L, progressKey ? progressKey : "");
  lua_setfield(L, -2, "progressKey");
  if (openError && openError[0]) {
    lua_pushstring(L, openError);
    lua_setfield(L, -2, "error");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "openFailed");
  } else {
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "openFailed");
  }
  // 0-based index into book TOC; Lua converts to 1-based when opening.
  if (switchChapterIndex >= 0) {
    lua_pushinteger(L, switchChapterIndex);
    lua_setfield(L, -2, "switchChapterIndex");
  }
  // Always stash for polling.
  lua_pushvalue(L, -1);
  lua_setglobal(L, "reader_last_progress");
  // table remains on stack as the single arg for onReaderClosed (optional).
  // onReaderClosed may persist progress through the provider's optional
  // network upload. Use the same bounded network window as loading draws.
  return callGlobal(L, &budget_, "onReaderClosed", 1, M4xLuaSandbox::kNetworkWallMs, errorOut);
}

bool M4xLuaHost::callOnTocClosed(bool cancelled, int chapterIndex0, const char* bookId, std::string& errorOut) {
  if (!L_) return true;
  auto* L = static_cast<lua_State*>(L_);
  lua_newtable(L);
  lua_pushboolean(L, cancelled ? 1 : 0);
  lua_setfield(L, -2, "cancelled");
  if (!cancelled && chapterIndex0 >= 0) {
    lua_pushinteger(L, chapterIndex0);  // 0-based (same as openToc currentIndex)
    lua_setfield(L, -2, "chapterIndex");
  }
  lua_pushstring(L, bookId ? bookId : "");
  lua_setfield(L, -2, "bookId");
  lua_pushvalue(L, -1);
  lua_setglobal(L, "reader_last_toc");
  return callGlobal(L, &budget_, "onTocClosed", 1, M4xLuaSandbox::kDefaultWallMs, errorOut);
}

bool M4xLuaHost::callProviderPump(std::string& errorOut) {
  if (M4xProgressiveLoader::session().needsPump()) {
    M4xProgressiveLoader::session().pump(120, 24 * 1024);
  }
  if (!L_) return true;
  auto* L = static_cast<lua_State*>(L_);
  lua_getglobal(L, "provider_pump_work");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  // Longer wall: may include one network hop + SD write for prefetch.
  return callGlobal(L, &budget_, "provider_pump_work", 0, M4xLuaSandbox::kDefaultWallMs * 4, errorOut);
}

bool M4xLuaHost::loaderNeedsPump() const {
  return M4xProgressiveLoader::session().needsPump();
}

static void jsonEscapeAppend(std::string& out, const char* s) {
  if (!s) return;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    const unsigned char c = *p;
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char hex[8];
      snprintf(hex, sizeof(hex), "\\u%04x", c);
      out += hex;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
}

static void luaGlobalString(lua_State* L, const char* key, std::string& out) {
  out.clear();
  if (!L || !key) return;
  lua_getglobal(L, key);
  if (lua_isstring(L, -1)) {
    const char* s = lua_tostring(L, -1);
    if (s) out = s;
  } else if (lua_isboolean(L, -1)) {
    out = lua_toboolean(L, -1) ? "true" : "false";
  } else if (lua_isnumber(L, -1)) {
    out = std::to_string(lua_tonumber(L, -1));
  }
  lua_pop(L, 1);
}

std::string M4xLuaHost::debugUiJson() const {
  // Compact JSON for m4adb automation (no screenshot OCR).
  std::string out = "{\"lua\":";
  auto* L = static_cast<lua_State*>(L_);
  if (!L) {
    out += "false}";
    return out;
  }
  out += "true";
  std::string screen, status, msgTitle, msgBody, msgCode, msgAction, msgHint;
  luaGlobalString(L, "screen", screen);
  luaGlobalString(L, "status_line", status);
  luaGlobalString(L, "message_title", msgTitle);
  luaGlobalString(L, "message_body", msgBody);
  luaGlobalString(L, "message_code", msgCode);
  luaGlobalString(L, "message_action", msgAction);
  luaGlobalString(L, "message_hint", msgHint);
  auto appendField = [&](const char* k, const std::string& v) {
    out += ",\"";
    out += k;
    out += "\":\"";
    jsonEscapeAppend(out, v.c_str());
    out += "\"";
  };
  appendField("screen", screen);
  appendField("status_line", status);
  appendField("message_title", msgTitle);
  appendField("message_body", msgBody);
  appendField("message_code", msgCode);
  appendField("message_action", msgAction);
  appendField("message_hint", msgHint);
  // Host-owned list scene (ui.listOpen) — titles of current page.
  const auto& sc = uiScene_;
  out += ",\"list_active\":";
  out += sc.active ? "true" : "false";
  if (sc.active) {
    appendField("list_title", sc.title);
    out += ",\"list_page\":";
    out += std::to_string(sc.page);
    out += ",\"list_page_size\":";
    out += std::to_string(sc.pageSize);
    out += ",\"list_rows\":[";
    const size_t n = sc.rows.size();
    const size_t maxShow = n > 12 ? 12 : n;
    for (size_t i = 0; i < maxShow; ++i) {
      if (i) out += ',';
      out += '"';
      jsonEscapeAppend(out, sc.rows[i].c_str());
      out += '"';
    }
    out += "],\"list_row_count\":";
    out += std::to_string(n);
  }
  out += '}';
  return out;
}

void M4xLuaHost::stop() {
  if (L_) {
    lua_close(static_cast<lua_State*>(L_));
    L_ = nullptr;
  }
  if (gHost == this) gHost = nullptr;
  renderer_ = nullptr;
  // Drop host-owned scene rows/file cursors with the Lua state.  Otherwise a
  // plugin error followed by a restart can retain a large SD-backed source
  // and leave the next app with a stale active scene.
  uiScene_ = UiListScene{};

  // Release Wi-Fi only if this host established the connection.
  // Never disconnect(true); never tear down a link owned by another component.
  if (wifiOwned_) {
    Serial.printf("[M4xNet] releasing owned Wi-Fi on host stop\n");
    WiFi.disconnect(false);
    wifiOwned_ = false;
  }
}

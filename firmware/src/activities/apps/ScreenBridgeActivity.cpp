#include "ScreenBridgeActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>

#include <Arduino.h>

#include "MappedInputManager.h"
#include "apps/providers/M4LanVisitorStore.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeWifi.h"
#include "apps/providers/M4Psram.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ErrorScreen.h"
#include "util/M4ScreenFrameCodec.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr const char* kEndpointRelPath = "/provider/endpoint.txt";
constexpr const char* kUserAgent = "Mozilla/5.0 Murphy-M4 ScreenBridge/1";

bool readFileSmall(const std::string& path, std::string& out) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("SB-Bridge", path.c_str(), f)) return false;
  char buf[64];
  while (f.available() && out.size() < 96) {
    const size_t want = std::min(sizeof(buf) - 1, 96 - out.size());
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), want);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  f.close();
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return !out.empty();
}

}  // namespace

ScreenBridgeActivity::ScreenBridgeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string appId, const std::function<void()>& onExit)
    : Activity("ScreenBridge", renderer, mappedInput),
      appId_(std::move(appId)),
      appDataRoot_("/apps_data/" + appId_),
      endpointPath_(appDataRoot_ + kEndpointRelPath),
      onExit_(onExit) {}

void ScreenBridgeActivity::onEnter() {
  Activity::onEnter();
  startWorker();
}

void ScreenBridgeActivity::onExit() {
  stopWorker();
  Activity::onExit();
}

std::string ScreenBridgeActivity::debugUiJson() {
  std::lock_guard<std::mutex> lock(mu_);
  const char* phaseName = "idle";
  switch (phase_) {
    case Phase::Discovering: phaseName = "discovering"; break;
    case Phase::Connecting: phaseName = "connecting"; break;
    case Phase::Ready: phaseName = "ready"; break;
    case Phase::Error: phaseName = "error"; break;
    default: break;
  }
  std::string cached;
  for (int i = 0; i < kSlots; ++i) {
    if (slots_[i].valid) {
      if (!cached.empty()) cached += ',';
      cached += std::to_string(slots_[i].page);
    }
  }
  return std::string("{\"kind\":\"screen_bridge\",\"phase\":\"") + phaseName +
         "\",\"base\":\"" + base_ + "\",\"page\":" + std::to_string(current_) +
         ",\"max_page\":" + std::to_string(maxPage_) + ",\"cached\":[" + cached +
         "],\"cache_enabled\":" + (cacheEnabled_ ? "true" : "false") +
         ",\"error\":\"" + error_ + "\"}";
}

// ---------------------------------------------------------------------------
// Worker task
// ---------------------------------------------------------------------------

void ScreenBridgeActivity::taskTrampoline(void* param) {
  static_cast<ScreenBridgeActivity*>(param)->taskLoop();
}

void ScreenBridgeActivity::taskLoop() {
  int32_t lastFailPage = -1;
  uint32_t lastFailMs = 0;
  int consecutiveFailures = 0;
  uint32_t lastDiscoveryMs = 0;
  uint32_t lastStatusMs = 0;

  while (true) {
    std::string base;
    bool haveBase = false;
    int32_t toConsume = -1;
    int32_t target = -1;
    int tapX = -1;
    int tapY = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      haveBase = !base_.empty();
      if (haveBase) {
        base = base_;
        if (cacheEnabled_ && consumePending_ != -1) {
          toConsume = consumePending_;
          consumePending_ = -1;
        }
        if (!cacheEnabled_ && tapPendingX_ >= 0) {
          tapX = tapPendingX_;
          tapY = tapPendingY_;
          tapPendingX_ = -1;
          tapPendingY_ = -1;
        }
        target = pickTargetLocked();
      }
    }
    if (stopped()) break;

    if (!haveBase) {
      const uint32_t now = millis();
      if (lastDiscoveryMs != 0 && now - lastDiscoveryMs < 2000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      lastDiscoveryMs = now;
      setPhase(Phase::Discovering, "");
      if (discoverEndpoint()) {
        consecutiveFailures = 0;
        setPhase(Phase::Connecting, "");
      } else if (!stopped()) {
        setPhase(Phase::Error, "未找到屏幕桥服务");
      }
      continue;
    }

    if (tapX >= 0) {
      if (sendTap(base, tapX, tapY)) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& s : slots_) {
          if (s.page == 0) s.valid = false;
        }
        current_ = 0;
        pendingTarget_ = -1;
      }
      continue;
    }

    if (toConsume != -1) sendConsume(base, toConsume);
    if (stopped()) break;

    const uint32_t statusNow = millis();
    if (lastStatusMs == 0 || statusNow - lastStatusMs >= 1000) {
      lastStatusMs = statusNow;
      (void)probeStatus(base);
      continue;
    }

    if (target < 0) {
      vTaskDelay(pdMS_TO_TICKS(60));
      continue;
    }
    if (target == lastFailPage && millis() - lastFailMs < 1500) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (fetchAndStore(base, target)) {
      lastFailPage = -1;
      lastFailMs = 0;
      consecutiveFailures = 0;
      std::lock_guard<std::mutex> lock(mu_);
      bool repaint = false;
      if (phase_ != Phase::Ready) {
        phase_ = Phase::Ready;
        error_.clear();
        repaint = true;
      }
      if (pendingTarget_ == target) {
        current_ = target;
        pendingTarget_ = -1;
        repaint = true;
      }
      if (target == current_) repaint = true;
      if (consumePending_ == -1 && target == current_) consumePending_ = target;
      if (repaint) ++paintSig_;
    } else if (!stopped()) {
      lastFailPage = target;
      lastFailMs = millis();
      ++consecutiveFailures;
      std::lock_guard<std::mutex> lock(mu_);
      if (consecutiveFailures >= 3) {
        // Phone may have left the network; fall back to discovery retry loop.
        phase_ = Phase::Error;
        error_ = "连接已断开，正在重试…";
        base_.clear();
      } else {
        error_ = "页面获取失败";
      }
      ++paintSig_;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& s : slots_) {
      M4Psram::freePrefer(s.data);
      s.data = nullptr;
      s.valid = false;
    }
#if defined(ARDUINO_ARCH_ESP32)
    task_ = nullptr;
#endif
  }
#if defined(ARDUINO_ARCH_ESP32)
  // The lock guard above must be destroyed before this non-returning call.
  M4Psram::deleteTask(nullptr);
#endif
}

void ScreenBridgeActivity::startWorker() {
#if defined(ARDUINO_ARCH_ESP32)
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = false;
  }
  TaskHandle_t h = nullptr;
  if (M4Psram::createTask(taskTrampoline, "ScreenBridge", 20u * 1024u, this, 1, &h) != pdPASS) {
    std::lock_guard<std::mutex> lock(mu_);
    phase_ = Phase::Error;
    error_ = "worker_task_failed";
    ++paintSig_;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    task_ = h;
  }
  setPhase(Phase::Discovering, "");
#else
  std::lock_guard<std::mutex> lock(mu_);
  phase_ = Phase::Error;
  error_ = "unsupported_platform";
  ++paintSig_;
#endif
}

void ScreenBridgeActivity::stopWorker() {
#if defined(ARDUINO_ARCH_ESP32)
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  // Join before Activity destruction. Every request carries a cancel callback,
  // so this normally completes promptly; returning early would leave a worker
  // with a dangling `this` pointer.
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (task_ == nullptr) break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
#endif
}

bool ScreenBridgeActivity::stopped() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stop_;
}

void ScreenBridgeActivity::setPhase(Phase phase, const std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  phase_ = phase;
  error_ = error;
  ++paintSig_;
}

// ---------------------------------------------------------------------------
// Endpoint discovery / persistence
// ---------------------------------------------------------------------------

bool ScreenBridgeActivity::discoverEndpoint() {
  std::string saved;
  if (readFileSmall(endpointPath_, saved) && probeStatus(saved)) {
    adopt(saved);
    return true;
  }
  if (stopped()) return false;

#if defined(ARDUINO_ARCH_ESP32)
  const auto wifi = M4NativeWifi::ensureConnected(15000u, [this]() { return stopped(); });
  if (!wifi.ok || stopped()) return false;
  const String ssid = WiFi.SSID();
  const std::vector<std::string> ips = M4LanVisitorStore::visitorsFor(ssid.c_str());
  for (const auto& ip : ips) {
    if (stopped()) return false;
    if (!M4LanVisitorStore::ipOk(ip.c_str())) continue;
    const std::string base = "http://" + ip + ":48624";
    if (probeStatus(base)) {
      adopt(base);
      return true;
    }
  }
#endif
  return false;
}

void ScreenBridgeActivity::adopt(const std::string& base) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    base_ = base;
    lastIp_ = base.substr(7);
    phase_ = Phase::Connecting;
    error_.clear();
    ++paintSig_;
  }
  persistEndpoint(base);
}

void ScreenBridgeActivity::persistEndpoint(const std::string& base) {
  if (endpointPath_.empty()) return;
  (void)M4NativeProviderIo::ensureParentDirs(endpointPath_);
  FsFile f;
  if (!SdMan.openFileForWrite("SB-Bridge", endpointPath_.c_str(), f)) return;
  (void)f.write(reinterpret_cast<const uint8_t*>(base.data()), base.size());
  f.sync();
  f.close();
}

bool ScreenBridgeActivity::probeStatus(const std::string& base) {
  M4NativeProviderHttp::Request req;
  req.method = "GET";
  req.url = base + "/v1/status";
  req.timeoutMs = 4000;
  req.maxBytes = 4096;
  req.headers = {{"User-Agent", kUserAgent}, {"Connection", "close"}};
  std::string body;
  M4NativeProviderHttp::Result net;
  const bool ok = M4NativeProviderHttp::requestSmall(req, body, net, req.maxBytes,
                                                     [this]() { return stopped(); });
  if (!ok || net.status < 200 || net.status >= 300) return false;

  const bool cacheEnabled = body.find("\"cacheEnabled\":false") == std::string::npos;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (cacheEnabled_ != cacheEnabled) {
      cacheEnabled_ = cacheEnabled;
      current_ = 0;
      pendingTarget_ = -1;
      consumePending_ = -1;
      tapPendingX_ = -1;
      tapPendingY_ = -1;
      for (auto& s : slots_) s.valid = false;
      ++paintSig_;
    }
  }

  // A live capture has no known final page count. Status `pages.hi` is only the
  // highest page captured so far and must not be treated as EOF.
  return true;
}

// ---------------------------------------------------------------------------
// Page fetch / cache
// ---------------------------------------------------------------------------

int32_t ScreenBridgeActivity::pickTargetLocked() {
  if (!cacheEnabled_) return slotCached(0) ? -1 : 0;
  if (pendingTarget_ >= 0 && !slotCached(pendingTarget_) &&
      (maxPage_ < 0 || pendingTarget_ <= maxPage_)) {
    return pendingTarget_;
  }
  if (current_ < 0) return -1;
  if (!slotCached(current_) && (maxPage_ < 0 || current_ <= maxPage_)) return current_;
  const int32_t ahead[] = {current_ + 1, current_ + 2, current_ - 1, current_ - 2};
  for (int32_t p : ahead) {
    if (p < 0) continue;
    if (maxPage_ >= 0 && p > maxPage_) continue;
    if (!slotCached(p)) return p;
  }
  return -1;
}

bool ScreenBridgeActivity::slotCached(int32_t page) const {
  for (const auto& s : slots_) {
    if (s.valid && s.page == page) return true;
  }
  return false;
}

ScreenBridgeActivity::CacheSlot* ScreenBridgeActivity::slotFor(int32_t page) {
  for (auto& s : slots_) {
    if (s.valid && s.page == page) return &s;
  }
  return nullptr;
}

ScreenBridgeActivity::CacheSlot* ScreenBridgeActivity::freeSlot() {
  for (auto& s : slots_) {
    if (!s.valid) return &s;
  }
  CacheSlot* best = nullptr;
  for (auto& s : slots_) {
    if (s.page == current_) continue;
    if (!best || s.seq < best->seq) best = &s;
  }
  return best;
}

bool ScreenBridgeActivity::fetchAndStore(const std::string& base, int32_t page) {
  M4NativeProviderHttp::Request req;
  req.method = "GET";
  req.url = base + "/v1/page?index=" + std::to_string(page);
  req.timeoutMs = 12000;
  req.maxBytes = 64u * 1024u;
  req.headers = {{"User-Agent", kUserAgent}, {"Connection", "close"}};
  std::string body;
  M4NativeProviderHttp::Result net;
  const bool ok = M4NativeProviderHttp::requestSmall(req, body, net, req.maxBytes,
                                                     [this]() { return stopped(); });
  if (!ok || body.size() < M4ScreenFrameCodec::kHeaderSize) return false;

  // Decode under the cache lock: the worker is the only slot writer and the
  // display copies under the same lock, so a slot is never read mid-write.
  std::lock_guard<std::mutex> lock(mu_);
  CacheSlot* slot = freeSlot();
  if (!slot) return false;
  if (!slot->data) {
    slot->data = static_cast<uint8_t*>(M4Psram::mallocPrefer(M4ScreenFrameCodec::kRawSize));
    if (!slot->data) return false;
  }
  const bool good = M4ScreenFrameCodec::decodePage(
      reinterpret_cast<const uint8_t*>(body.data()), body.size(), slot->data,
      M4ScreenFrameCodec::kRawSize);
  if (!good) {
    slot->valid = false;
    slot->page = -1;
    return false;
  }
  slot->page = page;
  slot->valid = true;
  slot->seq = ++nextSeq_;
  return true;
}

void ScreenBridgeActivity::sendConsume(const std::string& base, int32_t page) {
  M4NativeProviderHttp::Request req;
  req.method = "POST";
  req.url = base + "/v1/consume?index=" + std::to_string(page);
  req.timeoutMs = 4000;
  req.maxBytes = 2048;
  req.headers = {{"User-Agent", kUserAgent}, {"Connection", "close"}};
  std::string body;
  M4NativeProviderHttp::Result net;
  (void)M4NativeProviderHttp::requestSmall(req, body, net, req.maxBytes,
                                           [this]() { return stopped(); });
}

bool ScreenBridgeActivity::sendTap(const std::string& base, int x, int y) {
  M4NativeProviderHttp::Request req;
  req.method = "POST";
  req.url = base + "/v1/tap?x=" + std::to_string(x) + "&y=" + std::to_string(y);
  req.timeoutMs = 10000;
  req.maxBytes = 2048;
  req.headers = {{"User-Agent", kUserAgent}, {"Connection", "close"}};
  std::string body;
  M4NativeProviderHttp::Result net;
  const bool ok = M4NativeProviderHttp::requestSmall(req, body, net, req.maxBytes,
                                                     [this]() { return stopped(); });
  return ok && net.status >= 200 && net.status < 300 &&
         body.find("\"ok\":true") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Main-thread input / rendering
// ---------------------------------------------------------------------------

bool ScreenBridgeActivity::queueRealtimeTap(int x, int y) {
  std::lock_guard<std::mutex> lock(mu_);
  if (cacheEnabled_) return false;
  tapPendingX_ = std::max(0, std::min(479, x));
  tapPendingY_ = std::max(0, std::min(799, y));
  return true;
}

void ScreenBridgeActivity::handlePrev() {
  if (queueRealtimeTap(72, 400)) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (current_ <= 0) return;
  if (slotCached(current_ - 1)) {
    current_ -= 1;
    if (consumePending_ == -1) consumePending_ = current_;
  } else {
    pendingTarget_ = current_ - 1;
  }
  ++paintSig_;
}

void ScreenBridgeActivity::handleNext() {
  if (queueRealtimeTap(408, 400)) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (maxPage_ >= 0 && current_ >= maxPage_) return;
  if (slotCached(current_ + 1)) {
    current_ += 1;
    if (consumePending_ == -1) consumePending_ = current_;
  } else {
    pendingTarget_ = current_ + 1;
  }
  ++paintSig_;
}

void ScreenBridgeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasBackGesture()) {
    onExit_();
    return;
  }

  // E-paper refresh noise can leave a synthetic-looking tap in the touch FIFO.
  // Ignore only touch (not physical page keys) briefly after each panel update,
  // otherwise a refresh can trigger the next refresh indefinitely.
  const bool touchReady = static_cast<int32_t>(millis() - ignoreTouchUntilMs_) >= 0;
  if (mappedInput.hasTouch() && touchReady) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (!queueRealtimeTap(tx, ty)) {
        const auto zone = TouchHitGeometry::readerZoneFromPoint(
            tx, ty, renderer.getScreenWidth(), renderer.getScreenHeight());
        if (zone == TouchHitGeometry::ReaderZone::Prev) {
          handlePrev();
        } else if (zone == TouchHitGeometry::ReaderZone::Next) {
          handleNext();
        }
      }
    } else {
      bool cacheEnabled = true;
      {
        std::lock_guard<std::mutex> lock(mu_);
        cacheEnabled = cacheEnabled_;
      }
      if (cacheEnabled) {
        const auto sw = mappedInput.wasSwipe();
        if (sw == MappedInputManager::SwipeDir::Right) handlePrev();
        else if (sw == MappedInputManager::SwipeDir::Left) handleNext();
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    handlePrev();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    handleNext();
  }

  uint32_t sig = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sig = paintSig_;
  }
  if (sig != lastPaintSig_) {
    lastPaintSig_ = sig;
    renderNow();
  }
}

void ScreenBridgeActivity::renderNow() {
  const bool geometryOk = HalDisplay::BUFFER_SIZE == M4ScreenFrameCodec::kRawSize;
  bool haveFrame = false;
  bool loading = false;
  Phase phase = Phase::Idle;
  std::string base;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(mu_);
    phase = phase_;
    base = base_;
    error = error_;
    loading = (pendingTarget_ >= 0);
    CacheSlot* s = slotFor(current_);
    if (geometryOk && s && s->valid && s->data && renderer.getFrameBuffer()) {
      std::memcpy(renderer.getFrameBuffer(), s->data, M4ScreenFrameCodec::kRawSize);
      haveFrame = true;
    }
  }

  if (haveFrame) {
    if (loading) {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, renderer.getScreenHeight() - 60, "加载中…");
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    ignoreTouchUntilMs_ = millis() + 600;
    return;
  }
  renderStatusScreen(phase, base, error);
}

void ScreenBridgeActivity::renderStatusScreen(Phase phase, const std::string& base,
                                              const std::string& error) {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, "屏幕桥");

  std::string title;
  std::string sub;
  switch (phase) {
    case Phase::Idle:
      title = "未连接";
      sub = "请先打开手机端屏幕桥服务";
      break;
    case Phase::Discovering:
      title = "正在查找手机…";
      sub = "请确认手机与阅读器连接同一 Wi-Fi，并开启屏幕桥（端口 48624）";
      break;
    case Phase::Connecting:
      title = "正在连接手机";
      sub = base.empty() ? "" : base;
      break;
    case Phase::Ready:
      title = "已连接";
      sub = "正在获取页面…";
      break;
    case Phase::Error:
      title = "连接失败";
      sub = error.empty() ? "请检查手机端屏幕桥是否开启" : error;
      break;
  }

  if (phase == Phase::Error) {
    const auto labels = mappedInput.mapLabels("« 返回", "", "", "");
    std::vector<std::string> diag;
    M4ErrorScreen::appendCode(diag, error.empty() ? "screenbridge_error" : error);
    M4ErrorScreen::addKV(diag, "base: ", base);
    M4ErrorScreen::addKV(diag, "app: ", appId_);
    {
      std::lock_guard<std::mutex> lock(mu_);
      char b[64];
      std::snprintf(b, sizeof(b), "page: %d  pending: %d  max: %d", static_cast<int>(current_),
                    static_cast<int>(pendingTarget_), static_cast<int>(maxPage_));
      M4ErrorScreen::add(diag, b);
      std::string cache;
      for (int i = 0; i < kSlots; ++i) {
        if (slots_[i].valid) {
          if (!cache.empty()) cache += ' ';
          cache += std::to_string(slots_[i].page);
        }
      }
      if (!cache.empty()) M4ErrorScreen::addKV(diag, "cache: ", cache);
    }
#if defined(ARDUINO_ARCH_ESP32)
    if (WiFi.status() == WL_CONNECTED) {
      M4ErrorScreen::addKV(diag, "wifi_ip: ", std::string(WiFi.localIP().toString().c_str()));
      char rssi[32];
      std::snprintf(rssi, sizeof(rssi), "wifi_rssi: %d", WiFi.RSSI());
      M4ErrorScreen::add(diag, rssi);
    } else {
      M4ErrorScreen::add(diag, "wifi: not connected");
    }
#endif
    M4ErrorScreen::add(diag, "port: 48624  proto: /v1/status|/v1/page");
    auto snap = M4ErrorScreen::genericFail("屏幕桥", title, sub, diag, labels.btn1, "");
    M4ErrorScreen::paint(renderer, snap, true);
    ignoreTouchUntilMs_ = millis() + 600;
    return;
  }

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 50;
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, y, title.c_str(), true, EpdFontFamily::BOLD);
  y += 44;
  if (!sub.empty()) M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, sub.c_str());
  y += 32;

  {
    std::lock_guard<std::mutex> lock(mu_);
    std::string cache;
    for (int i = 0; i < kSlots; ++i) {
      if (slots_[i].valid) {
        if (!cache.empty()) cache += ' ';
        cache += std::to_string(slots_[i].page);
      }
    }
    if (!cache.empty()) {
      M4UiText::drawMuted(renderer, UI_10_FONT_ID, metrics.contentSidePadding, y,
                          ("已缓存: " + cache).c_str());
    }
  }

  const auto labels = mappedInput.mapLabels("« 返回", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  ignoreTouchUntilMs_ = millis() + 600;
}

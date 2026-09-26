#include "AppStoreActivity.h"

#include "AppInstallActivity.h"
#include "MappedInputManager.h"
#include "apps/M4HttpTransport.h"
#include "apps/M4xInstaller.h"
#include "apps/M4xJsonStream.h"
#include "apps/M4xRegistry.h"
#include "apps/providers/M4NativeWifi.h"
#include "apps/providers/M4Psram.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4RenderGuard.h"
#include "util/M4UiText.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

extern SemaphoreHandle_t gM4RenderMutex;

namespace {
constexpr const char* kCatalogPages = "https://einklover.github.io/m4-stack/appstore/index.json";
constexpr const char* kCatalogRaw = "https://raw.githubusercontent.com/einklover/m4-stack/gh-pages/appstore/index.json";
constexpr const char* kCatalogCache = "/system/appstore_catalog.json";
constexpr const char* kPackagePrefix = "https://einklover.github.io/m4-stack/appstore/packages/";
constexpr size_t kMaxCatalogBytes = 48u * 1024u;
constexpr size_t kMaxPackageBytes = 2u * 1024u * 1024u;
constexpr int kVisibleRows = 6;
constexpr int kRowTop = 116;
constexpr int kRowHeight = 94;

bool hasPrefix(const std::string& value, const char* prefix) {
  return value.compare(0, std::strlen(prefix), prefix) == 0;
}
bool validSha(const std::string& hex) {
  if (hex.size() != 64) return false;
  for (unsigned char c : hex) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
class TextSink final : public M4xJsonStream::Sink {
 public:
  std::string text;
  bool write(const uint8_t* bytes, size_t len) override {
    if (len > kMaxCatalogBytes - text.size()) return false;
    text.append(reinterpret_cast<const char*>(bytes), len);
    return true;
  }
};
class FileHashSink final : public M4xJsonStream::Sink {
 public:
  explicit FileHashSink(FsFile& file) : file_(file) {
    mbedtls_sha256_init(&ctx_);
    initialized_ = mbedtls_sha256_starts(&ctx_, 0) == 0;
  }
  ~FileHashSink() override { mbedtls_sha256_free(&ctx_); }
  bool write(const uint8_t* bytes, size_t len) override {
    if (!initialized_ || failed_ || len > kMaxPackageBytes - size_) return false;
    if (file_.write(bytes, len) != len || mbedtls_sha256_update(&ctx_, bytes, len) != 0) {
      failed_ = true;
      return false;
    }
    size_ += len;
    return true;
  }
  std::string checksum() {
    if (!initialized_ || failed_ || size_ == 0) return {};
    uint8_t digest[32]{};
    if (mbedtls_sha256_finish(&ctx_, digest) != 0) return {};
    constexpr char h[] = "0123456789abcdef";
    std::string result(64, '0');
    for (int i=0;i<32;++i) {
      result[2*i] = h[digest[i] >> 4];
      result[2*i+1] = h[digest[i] & 15];
    }
    return result;
  }
 private:
  FsFile& file_;
  mbedtls_sha256_context ctx_{};
  size_t size_ = 0;
  bool initialized_ = false;
  bool failed_ = false;
};

bool readCache(std::string& out) {
  FsFile file;
  if (!SdMan.openFileForRead("AppStore", kCatalogCache, file)) return false;
  out.clear();
  std::array<uint8_t, 512> buf{};
  while (file.available()) {
    int n = file.read(buf.data(), buf.size());
    if (n <= 0 || static_cast<size_t>(n) > kMaxCatalogBytes - out.size()) {
      file.close();
      return false;
    }
    out.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
  }
  file.close();
  return !out.empty();
}
bool cancelled(void* ctx) {
  return static_cast<std::atomic<bool>*>(ctx)->load(std::memory_order_acquire);
}
}  // namespace

struct AppStoreActivity::State {
  SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  std::vector<App> apps;
  std::string message = "正在读取目录...";
  std::string packagePath;
  std::atomic<uint32_t> revision{0};
  std::atomic<bool> cancel{false};
  std::atomic<bool> busy{false};
  std::atomic<bool> downloadDone{false};
  bool downloadOk = false;
  bool offline = false;
  ~State() { if (mutex) vSemaphoreDelete(mutex); }
  void publish(std::vector<App> updated, std::string info, bool cached) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    apps = std::move(updated);
    message = std::move(info);
    offline = cached;
    revision.fetch_add(1, std::memory_order_release);
    xSemaphoreGive(mutex);
  }
  void finishDownload(bool ok, const std::string& path, const char* messageText) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    downloadOk = ok;
    packagePath = ok ? path : "";
    message = messageText;
    revision.fetch_add(1, std::memory_order_release);
    xSemaphoreGive(mutex);
    downloadDone.store(true, std::memory_order_release);
  }
};

struct AppStoreActivity::Job {
  std::shared_ptr<State> state;
  bool download = false;
  App app;
};

void AppStoreActivity::taskEntry(void* arg) {
  std::unique_ptr<Job> job(static_cast<Job*>(arg));
  auto s = job->state;  // Shared ownership; never dereference the Activity from this task.
  if (!job->download) {
    std::string body;
    std::vector<App> parsed;
    std::string error = "无法获取应用目录";
    bool online = false;
    if (!s->cancel.load(std::memory_order_acquire)) {
      if (!M4NativeWifi::isReady()) {
        (void)M4NativeWifi::ensureConnected(8000, [&] { return s->cancel.load(); });
      }
      if (M4NativeWifi::isReady() && !s->cancel.load()) {
        for (const char* url : {kCatalogPages, kCatalogRaw}) {
          TextSink sink;
          M4HttpTransport::Request req;
          req.url = url;
          req.maxBytes = kMaxCatalogBytes;
          req.timeoutMs = 12000;
          auto result = M4HttpTransport::requestToSink(req, sink, nullptr, nullptr,
                                                       cancelled, &s->cancel);
          if (result.ok) { body = std::move(sink.text); online = true; break; }
          error = result.error;
          if (s->cancel.load()) break;
        }
      } else error = "未联网";
    }

    auto parse = [&](const std::string& json) -> bool {
      JsonDocument doc;
      if (deserializeJson(doc, json)) return false;
      if ((doc["schemaVersion"] | 0) != 1 || !doc["apps"].is<JsonArray>()) return false;
      std::vector<App> tmp;
      for (JsonObject item : doc["apps"].as<JsonArray>()) {
        App a;
        a.id = item["id"] | "";
        a.name = item["name"] | "";
        a.version = item["version"] | "";
        a.versionCode = item["versionCode"] | 0;
        a.description = item["description"] | "";
        a.category = item["category"] | "";
        a.packageUrl = item["packageUrl"] | "";
        a.sha256 = item["sha256"] | "";
        a.sourceUrl = item["sourceUrl"] | "";
        if (!M4xIsValidPackageId(a.id) || a.name.empty() || a.name.size() > 80 ||
            a.description.size() > 240 || a.versionCode <= 0 ||
            !hasPrefix(a.packageUrl, kPackagePrefix) || !validSha(a.sha256)) return false;
        if (tmp.size() >= 48) return false;
        for (const auto& prev : tmp) if (prev.id == a.id) return false;
        tmp.push_back(std::move(a));
      }
      if (tmp.empty()) return false;
      parsed = std::move(tmp);
      return true;
    };

    if (online && !parse(body)) { online = false; error = "目录格式或签名信息无效"; }
    if (online) {
      M4xInstaller::ensureLayout();
      (void)SdMan.writeFile(kCatalogCache, String(body.c_str()));
      if (!s->cancel.load()) s->publish(std::move(parsed), "目录已更新", false);
    } else {
      std::string cached;
      if (readCache(cached) && parse(cached)) {
        if (!s->cancel.load()) s->publish(std::move(parsed), "离线目录：可能不是最新版", true);
      } else if (!s->cancel.load()) s->publish({}, error, true);
    }
    s->busy.store(false, std::memory_order_release);
    M4Psram::deleteTask(nullptr);
    return;
  }

  bool ok = false;
  std::string dest;
  const App& app = job->app;
  const char* messageText = "下载失败";
  if (M4NativeWifi::isReady() && !s->cancel.load() &&
      M4xIsValidPackageId(app.id) && hasPrefix(app.packageUrl, kPackagePrefix) &&
      validSha(app.sha256)) {
    M4xInstaller::ensureLayout();
    dest = std::string("/apps_inbox/store-") + app.id + ".m4x";
    FsFile file;
    if (SdMan.openFileForWrite("AppStore", dest.c_str(), file)) {
      FileHashSink sink(file);
      M4HttpTransport::Request req;
      req.url = app.packageUrl.c_str();
      req.maxBytes = kMaxPackageBytes;
      req.timeoutMs = 45000;
      req.followRedirects = false;  // Same-host Pages asset: no cross-host TLS redirect.
      const auto result = M4HttpTransport::requestToSink(req, sink, nullptr, nullptr,
                                                         cancelled, &s->cancel);
      const std::string digest = result.ok ? sink.checksum() : "";
      file.sync();
      file.close();
      ok = result.ok && digest == app.sha256 && !s->cancel.load();
      messageText = !result.ok ? "网络下载失败" : digest != app.sha256 ? "SHA-256 校验失败" :
                    s->cancel.load() ? "已取消" : "下载校验成功";
      // A valid checksum authenticates bytes, not the catalog identity.
      // Reject accidentally mislabeled packages before invoking the installer.
      if (ok) {
        const M4xInstallResult probe = M4xInstaller::probe(dest);
        if (!probe.ok || probe.manifest.id != app.id ||
            probe.manifest.versionCode != app.versionCode) {
          ok = false;
          messageText = "安装包标识或版本与商店目录不一致";
        }
      }
      if (!ok) SdMan.remove(dest.c_str());
    } else messageText = "SD 卡无法写入";
  } else messageText = "请先连接 Wi-Fi";
  s->finishDownload(ok, dest, messageText);
  s->busy.store(false, std::memory_order_release);
  M4Psram::deleteTask(nullptr);
}

void AppStoreActivity::startJob(bool download, const App* app) {
  if (!state_ || state_->busy.exchange(true, std::memory_order_acq_rel)) return;
  auto* job = new Job{state_, download, app ? *app : App{}};
  if (download) {
    state_->downloadDone.store(false, std::memory_order_release);
    handoff_ = false;
    status_ = "正在下载并校验...";
    dirty_ = true;
  } else {
    status_ = "正在刷新目录...";
    dirty_ = true;
  }
  if (M4Psram::createTask(&AppStoreActivity::taskEntry, "M4AppStore", 12288, job, 1, nullptr) != pdPASS) {
    delete job;
    state_->busy.store(false, std::memory_order_release);
    status_ = "可用内存不足";
    dirty_ = true;
  }
}

void AppStoreActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  state_ = std::make_shared<State>();
  installed_.clear();
  for (const auto& app : M4xRegistry::load()) installed_.emplace_back(app.id, app.versionCode);
  startJob(false);
}

void AppStoreActivity::onExit() {
  if (state_) state_->cancel.store(true, std::memory_order_release);
  ActivityWithSubactivity::onExit();
  state_.reset();  // Workers hold their own shared State until HTTPS/SD shutdown.
}

void AppStoreActivity::refreshSnapshot() {
  if (!state_) return;
  const uint32_t rev = state_->revision.load(std::memory_order_acquire);
  if (rev == seenRevision_) return;
  xSemaphoreTake(state_->mutex, portMAX_DELAY);
  shown_ = state_->apps;
  status_ = state_->message;
  offline_ = state_->offline;
  xSemaphoreGive(state_->mutex);
  seenRevision_ = rev;
  if (selected_ >= static_cast<int>(shown_.size())) selected_ = std::max(0, static_cast<int>(shown_.size())-1);
  dirty_ = true;
}

void AppStoreActivity::select() {
  if (selected_ < 0 || selected_ >= static_cast<int>(shown_.size()) || !state_) return;
  if (!detail_) { detail_ = true; dirty_ = true; return; }
  if (state_->busy.load()) return;
  const App app = shown_[static_cast<size_t>(selected_)];
  for (const auto& installed : installed_) {
    if (installed.first == app.id && installed.second >= app.versionCode) return;
  }
  startJob(true, &app);
}

void AppStoreActivity::loop() {
  reapRetiredSubActivities();
  activatePendingSubActivity();
  if (subActivity) {
    if (pumpSubActivityFrame()) {
      installed_.clear();
      for (const auto& app : M4xRegistry::load()) installed_.emplace_back(app.id,app.versionCode);
      handoff_ = false;
      if (state_) state_->downloadDone.store(false, std::memory_order_release);
      detail_ = false;
      dirty_ = true;
    }
    return;
  }
  refreshSnapshot();
  if (state_ && state_->downloadDone.load(std::memory_order_acquire) && !handoff_) {
    handoff_ = true;
    std::string path;
    bool ok = false;
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    ok = state_->downloadOk;
    path = state_->packagePath;
    xSemaphoreGive(state_->mutex);
    if (ok && !path.empty()) {
      // Existing installer owns package probe, permission confirmation and install.
      enterNewActivity(new AppInstallActivity(renderer, mappedInput, path,
                                              [this]() { requestExitSubActivity(); }));
      return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    if (detail_) { detail_ = false; dirty_ = true; }
    else if (onBack_) onBack_();
    return;
  }
  if (state_ && !state_->busy.load(std::memory_order_acquire)) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !detail_) startJob(false);
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) && !detail_ && !shown_.empty()) {
      selected_ = (selected_ + 1) % static_cast<int>(shown_.size());
      dirty_ = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) && !detail_ && !shown_.empty()) {
      selected_ = (selected_ + static_cast<int>(shown_.size()) - 1) % static_cast<int>(shown_.size());
      dirty_ = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) select();
    int x=0,y=0;
    if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(x,y)) {
      if (detail_) {
        if (y >= renderer.getScreenHeight()-160) select();
      } else if (y >= kRowTop && y < kRowTop + kVisibleRows*kRowHeight) {
        int idx = page_*kVisibleRows+(y-kRowTop)/kRowHeight;
        if (idx >= 0 && idx < static_cast<int>(shown_.size())) {
          if (idx == selected_) select();
          else { selected_=idx; dirty_=true; }
        }
      }
    }
  }
  if (dirty_) {
    M4RenderGuard guard(gM4RenderMutex);
    if (guard.owns()) {
      render();
      dirty_ = false;
    }
  }
}

void AppStoreActivity::render() {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0,metrics.topPadding,w,metrics.headerHeight},"应用商店");
  if (detail_ && selected_ >= 0 && selected_ < static_cast<int>(shown_.size())) {
    const App& app = shown_[static_cast<size_t>(selected_)];
    M4UiText::draw(renderer,UI_12_FONT_ID,24,115,app.name.c_str(),true);
    std::string version = "版本 " + app.version;
    M4UiText::draw(renderer,UI_10_FONT_ID,24,160,version.c_str());
    M4UiText::draw(renderer,UI_10_FONT_ID,24,204,app.category.c_str());
    // Description is intentionally bounded before catalog acceptance.
    std::string desc = app.description.substr(0, 72);
    M4UiText::draw(renderer,UI_10_FONT_ID,24,255,desc.c_str());
    bool installed = false, update = false;
    for (const auto& entry : installed_) if (entry.first == app.id) {
      installed = true; update = entry.second < app.versionCode; break;
    }
    const char* button = installed && !update ? "已安装" : update ? "更新应用" : "下载安装";
    renderer.drawRoundedRect(38,h-178,w-76,70,1,10,true);
    M4UiText::drawCenteredInBox(renderer,UI_12_FONT_ID,38,h-178,w-76,70,button,true);
    M4UiText::draw(renderer,UI_10_FONT_ID,24,h-220,status_.c_str());
    GUI.drawButtonHints(renderer,"返回",button,"","");
  } else {
    if (shown_.empty()) {
      M4UiText::drawCentered(renderer,UI_12_FONT_ID,210,status_.c_str());
      M4UiText::drawCentered(renderer,UI_10_FONT_ID,260,"请连接 Wi-Fi 后刷新");
    } else {
      page_ = selected_/kVisibleRows;
      for (int row=0;row<kVisibleRows;++row) {
        const int i=page_*kVisibleRows+row;
        if (i>=static_cast<int>(shown_.size())) break;
        const int y=kRowTop+row*kRowHeight;
        renderer.drawRoundedRect(18,y,w-36,kRowHeight-8,1,8,true);
        if (i==selected_) renderer.drawRoundedRect(21,y+3,w-42,kRowHeight-14,1,7,true);
        const App& app=shown_[static_cast<size_t>(i)];
        M4UiText::draw(renderer,UI_12_FONT_ID,34,y+17,app.name.c_str(),true);
        std::string subtitle=app.version+"  "+app.category;
        for (const auto& old:installed_) if(old.first==app.id) {
          subtitle+=old.second<app.versionCode?"  可更新":"  已安装";break;
        }
        M4UiText::draw(renderer,UI_10_FONT_ID,34,y+52,subtitle.c_str());
      }
    }
    if (!status_.empty()) {
      std::string label=offline_?"离线缓存":state_ && state_->busy.load()?"正在同步":status_;
      M4UiText::draw(renderer,UI_10_FONT_ID,22,h-93,label.c_str());
    }
    GUI.drawButtonHints(renderer,"返回","详情","","刷新");
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  (void)renderer.storeLastShown();
}

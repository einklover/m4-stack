#include "apps/native/M4ScreenBridgeController.h"

#include "apps/providers/M4LanVisitorStore.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeWifi.h"
#include "apps/providers/M4Psram.h"
#include "qemu/M4QemuNet.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace M4NativeAppControllers {
namespace {

constexpr const char* kUserAgent = "Murphy-M4 ScreenBridge/2";

std::string urlEncode(const std::string& value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

bool readSmallFile(const std::string& path, std::string& out) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("SB-V2", path.c_str(), f)) return false;
  char buf[64];
  while (f.available() && out.size() < 128) {
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  f.close();
  while (!out.empty() && static_cast<unsigned char>(out.back()) <= ' ') out.pop_back();
  return !out.empty();
}

class ScreenBridgeController final : public M4NativeUi::Controller {
 public:
  explicit ScreenBridgeController(M4xInstalledApp app)
      : app_(std::move(app)), endpointPath_("/apps_data/" + app_.id + "/provider/endpoint.txt") {}

  ~ScreenBridgeController() override { stopWorker(); }

  bool scalar(const std::string& key, std::string& out) const override {
    std::lock_guard<std::mutex> lock(mu_);
    if (key == "app.name") out = app_.name;
    else if (key == "bridge.status") out = status_;
    else if (key == "bridge.noteTitle") out = noteTitle_.empty() ? "小红书笔记" : noteTitle_;
    else if (key == "bridge.noteMeta") {
      out = noteAuthor_;
      if (imageCount_ > 0) out += (out.empty() ? "" : " · ") + std::to_string(imageCount_) + " 图";
      if (!likes_.empty()) out += (out.empty() ? "" : " · ") + likes_ + " 赞";
      if (!commentCount_.empty()) out += (out.empty() ? "" : " · ") + commentCount_ + " 评";
    } else if (key == "bridge.noteBody") out = noteBody_;
    else if (key == "bridge.imagePath") out = imagePath_;
    else if (key == "bridge.imageMeta") {
      out = imageCount_ > 0 ? std::to_string(imageIndex_ + 1) + " / " + std::to_string(imageCount_) : "暂无图片";
    } else if (key == "bridge.commentsText") out = commentsText_;
    else return false;
    return true;
  }

  size_t rowCount(const std::string& source) const override {
    std::lock_guard<std::mutex> lock(mu_);
    if (source == "bridge.apps") return apps_.size();
    if (source == "bridge.feed") return feed_.size();
    if (source == "bridge.comments") return comments_.size();
    return 0;
  }

  bool rowAt(const std::string& source, size_t index0, M4NativeUi::Row& out) const override {
    std::lock_guard<std::mutex> lock(mu_);
    const std::vector<M4NativeUi::Row>* rows = nullptr;
    if (source == "bridge.apps") rows = &apps_;
    else if (source == "bridge.feed") rows = &feed_;
    else if (source == "bridge.comments") rows = &comments_;
    if (!rows || index0 >= rows->size()) return false;
    out = (*rows)[index0];
    return true;
  }

  M4NativeUi::ActionResult dispatch(const std::string& action,
                                    const M4NativeUi::ActionContext& ctx) override {
    if (action == "system.close") return M4NativeUi::ActionResult::close();
    if (action == "system.back") {
      if (ctx.screenId == "comments") return M4NativeUi::ActionResult::navigate("note");
      if (ctx.screenId == "images") return M4NativeUi::ActionResult::navigate("note");
      if (ctx.screenId == "note") return M4NativeUi::ActionResult::navigate("xhs");
      if (ctx.screenId == "xhs") return M4NativeUi::ActionResult::navigate("home");
      return M4NativeUi::ActionResult::close();
    }
    if (action == "bridge.refreshApps") {
      queue(Job::LoadApps);
      return M4NativeUi::ActionResult::repaint();
    }
    if (action == "bridge.refreshFeed") {
      queue(Job::LoadFeed);
      return M4NativeUi::ActionResult::repaint();
    }
    if (action == "bridge.moreFeed") {
      queue(Job::LoadFeed);
      return M4NativeUi::ActionResult::repaint();
    }
    if (action == "bridge.moreComments") {
      queue(Job::MoreComments);
      return M4NativeUi::ActionResult::repaint();
    }
    if (action == "bridge.showComments") {
      queue(Job::OpenComments);
      return M4NativeUi::ActionResult::navigate("comments");
    }
    if (action == "bridge.showImages") {
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (imageCount_ <= 0) {
          status_ = "这篇笔记没有图片";
          ++revision_;
          return M4NativeUi::ActionResult::repaint();
        }
      }
      queue(Job::LoadImage, "0");
      return M4NativeUi::ActionResult::navigate("images");
    }
    if (action == "bridge.imagePrev" || action == "bridge.imageNext") {
      int next = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);
        next = imageIndex_ + (action == "bridge.imageNext" ? 1 : -1);
        next = std::max(0, std::min(std::max(0, imageCount_ - 1), next));
      }
      queue(Job::LoadImage, std::to_string(next));
      return M4NativeUi::ActionResult::repaint();
    }
    if (action == "bridge.openApp" && !ctx.rowKey.empty()) {
      if (ctx.rowKey == "com.xingin.xhs") {
        queue(Job::OpenXhs, ctx.rowKey);
        return M4NativeUi::ActionResult::navigate("xhs");
      }
      queue(Job::OpenApp, ctx.rowKey);
      return M4NativeUi::ActionResult::openScreenBridge();
    }
    if (action == "bridge.openNote" && !ctx.rowKey.empty()) {
      queue(Job::OpenNote, ctx.rowKey);
      return M4NativeUi::ActionResult::navigate("note");
    }
    M4NativeUi::ActionResult r;
    r.kind = M4NativeUi::ActionKind::Error;
    r.error = "screenbridge_action_unsupported";
    return r;
  }

  uint32_t revision() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return revision_;
  }

  void pollAsync() override {
    startWorker();
    std::lock_guard<std::mutex> lock(mu_);
    if (!initialQueued_) {
      initialQueued_ = true;
      pending_ = Job::LoadApps;
    }
  }

 private:
  enum class Job : uint8_t {
    None = 0, LoadApps, OpenApp, OpenXhs, LoadFeed, OpenNote, OpenComments, MoreComments, LoadImage
  };

  static void taskMain(void* arg) { static_cast<ScreenBridgeController*>(arg)->workerLoop(); }

  void startWorker() {
#if defined(ARDUINO_ARCH_ESP32)
    std::lock_guard<std::mutex> lock(mu_);
    if (task_ || stop_) return;
    TaskHandle_t handle = nullptr;
    if (M4Psram::createTask(taskMain, "ScreenBridgeV2", 24u * 1024u, this, 1, &handle) == pdPASS) {
      task_ = handle;
    } else {
      status_ = "无法启动屏幕桥任务";
      ++revision_;
    }
#endif
  }

  void stopWorker() {
#if defined(ARDUINO_ARCH_ESP32)
    TaskHandle_t handle = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
      handle = task_;
    }
    for (int i = 0; handle && i < 100; ++i) {
      vTaskDelay(pdMS_TO_TICKS(20));
      std::lock_guard<std::mutex> lock(mu_);
      handle = task_;
    }
    if (handle) M4Psram::deleteTask(handle);
    std::lock_guard<std::mutex> lock(mu_);
    task_ = nullptr;
#endif
  }

  void queue(Job job, std::string key = {}) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_ = job;
    selectedKey_ = std::move(key);
    status_ = "加载中…";
    ++revision_;
  }

  void workerLoop() {
    while (true) {
      Job job = Job::None;
      std::string key;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) break;
        job = pending_;
        pending_ = Job::None;
        key = selectedKey_;
      }
      if (job == Job::None) {
        vTaskDelay(pdMS_TO_TICKS(40));
        continue;
      }
      run(job, key);
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      task_ = nullptr;
    }
    M4Psram::deleteTask(nullptr);
  }

  void run(Job job, const std::string& key) {
    bool ok = false;
    if (job == Job::LoadApps) ok = loadApps();
    else if (job == Job::OpenApp) ok = post("/v2/apps/open?id=" + urlEncode(key));
    else if (job == Job::OpenXhs) {
      ok = post("/v2/apps/open?id=com.xingin.xhs");
      if (ok) {
        vTaskDelay(pdMS_TO_TICKS(1800));
        ok = loadFeed();
      }
    } else if (job == Job::LoadFeed) ok = loadFeed();
    else if (job == Job::OpenNote) {
      ok = post("/v2/xhs/feed/open?token=" + urlEncode(key));
      if (ok) {
        vTaskDelay(pdMS_TO_TICKS(900));
        ok = loadNote();
      }
    } else if (job == Job::OpenComments) {
      ok = post("/v2/xhs/comments/open");
      if (ok) {
        vTaskDelay(pdMS_TO_TICKS(700));
        ok = loadComments(false);
      }
    } else if (job == Job::MoreComments) ok = loadComments(true);
    else if (job == Job::LoadImage) ok = loadImage(std::max(0, std::atoi(key.c_str())));

    std::lock_guard<std::mutex> lock(mu_);
    if (job == Job::OpenApp) status_ = ok ? "已在手机打开 · 此应用暂用屏幕镜像" : "手机应用打开失败";
    else if (!ok && status_.find("失败") == std::string::npos) status_ = "手机内容读取失败";
    ++revision_;
  }

  bool ensureEndpoint() {
    if (!base_.empty()) return true;
#ifdef M4_SCREEN_BRIDGE_ENDPOINT
    base_ = M4_SCREEN_BRIDGE_ENDPOINT;
    return probe();
#else
    std::string saved;
    if (readSmallFile(endpointPath_, saved)) {
      base_ = saved;
      if (probe()) return true;
      base_.clear();
    }
    const auto wifi = M4NativeWifi::ensureConnected(15000, [this]() {
      std::lock_guard<std::mutex> lock(mu_);
      return stop_;
    });
    if (!wifi.ok) return false;
    const std::string ssid = M4QemuNet::ssidStd();
    for (const auto& ip : M4LanVisitorStore::visitorsFor(ssid.c_str())) {
      base_ = "http://" + ip + ":48624";
      if (probe()) return true;
    }
    base_.clear();
    return false;
#endif
  }

  bool probe() {
    std::string body;
    return request("GET", "/v1/status", body, 4096);
  }

  bool request(const char* method, const std::string& path, std::string& body, size_t cap) {
    if (base_.empty() && path != "/v1/status" && !ensureEndpoint()) return false;
    M4NativeProviderHttp::Request req;
    req.method = method;
    req.url = base_ + path;
    req.timeoutMs = 20000;
    req.maxBytes = cap;
    req.headers = {{"User-Agent", kUserAgent}, {"Connection", "close"}};
    M4NativeProviderHttp::Result net;
    const bool ok = M4NativeProviderHttp::requestSmall(req, body, net, cap, [this]() {
      std::lock_guard<std::mutex> lock(mu_);
      return stop_;
    });
    return ok && net.status >= 200 && net.status < 300 && body.find("\"ok\":false") == std::string::npos;
  }

  bool post(const std::string& path) {
    if (!ensureEndpoint()) return false;
    std::string body;
    return request("POST", path, body, 2048);
  }

  bool loadApps() {
    if (!ensureEndpoint()) return setFailure("未找到手机屏幕桥");
    std::string body;
    if (!request("GET", "/v2/apps", body, 64u * 1024u)) return setFailure("手机应用列表读取失败");
    JsonDocument doc;
    if (deserializeJson(doc, body)) return setFailure("手机应用列表格式错误");
    std::vector<M4NativeUi::Row> rows;
    for (JsonObject item : doc["apps"].as<JsonArray>()) {
      M4NativeUi::Row row;
      row.key = item["id"] | "";
      row.title = item["title"] | row.key;
      row.subtitle = item["subtitle"] | "";
      if (!row.key.empty()) rows.push_back(std::move(row));
    }
    std::lock_guard<std::mutex> lock(mu_);
    apps_ = std::move(rows);
    status_ = apps_.empty() ? "手机未返回可启动应用" : "已连接手机 · " + std::to_string(apps_.size()) + " 个应用";
    ++revision_;
    return !apps_.empty();
  }

  bool loadFeed() {
    if (!ensureEndpoint()) return setFailure("未找到手机屏幕桥");
    for (int pass = 0; pass < 8; ++pass) {
      std::string body;
      if (!request("GET", "/v2/xhs/feed", body, 64u * 1024u)) return setFailure("小红书推荐读取失败");
      JsonDocument doc;
      if (deserializeJson(doc, body)) return setFailure("小红书推荐格式错误");
      std::vector<M4NativeUi::Row> rows;
      for (JsonObject item : doc["items"].as<JsonArray>()) {
        M4NativeUi::Row row;
        row.key = item["token"] | "";
        row.title = item["title"] | "";
        const std::string author = item["author"] | "";
        const std::string likes = item["likes"] | "";
        row.subtitle = author + (likes.empty() ? "" : " · " + likes + " 赞");
        if (!row.key.empty() && !row.title.empty()) rows.push_back(std::move(row));
      }
      const bool collecting = doc["collecting"] | false;
      const int cached = doc["cached"] | static_cast<int>(rows.size());
      const int target = doc["target"] | 24;
      const std::string state = doc["state"] | "other";
      {
        std::lock_guard<std::mutex> lock(mu_);
        feed_ = std::move(rows);
        status_ = "图文 " + std::to_string(cached) + "/" + std::to_string(target)
            + (collecting ? " · 手机后台采集中" : " · 已缓存，可离线浏览")
            + (state == "home" ? "" : " · 正在导航");
        ++revision_;
      }
      if (!collecting || cached >= target) return true;
      vTaskDelay(pdMS_TO_TICKS(900));
    }
    return true;
  }

  bool loadNote() {
    std::string body;
    if (!request("GET", "/v2/xhs/note", body, 48u * 1024u)) return setFailure("笔记正文读取失败");
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["readable"].as<bool>()) return setFailure("该笔记没有可读正文");
    std::lock_guard<std::mutex> lock(mu_);
    noteTitle_ = doc["title"] | "";
    noteAuthor_ = doc["author"] | "";
    noteBody_ = doc["body"] | "";
    commentCount_ = doc["commentCount"] | "";
    likes_ = doc["likes"] | "";
    collects_ = doc["collects"] | "";
    timestamp_ = doc["timestamp"] | "";
    imageCount_ = doc["imageCount"] | 0;
    imageIndex_ = doc["imageIndex"] | 0;
    status_ = "正文已整理为电子书分页";
    ++revision_;
    return !noteBody_.empty();
  }

  bool loadComments(bool advance) {
    std::vector<M4NativeUi::Row> rows;
    size_t baseline = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      baseline = comments_.size();
    }
    bool collecting = false;
    for (int pass = 0; pass < 8; ++pass) {
      std::string body;
      if (!request("GET", std::string("/v2/xhs/comments?advance=") + (advance ? "1" : "0"),
                   body, 48u * 1024u)) return setFailure("评论读取失败");
      JsonDocument doc;
      if (deserializeJson(doc, body)) return setFailure("评论格式错误");
      rows.clear();
      for (JsonObject item : doc["items"].as<JsonArray>()) {
        M4NativeUi::Row row;
        row.key = std::to_string(rows.size());
        row.title = item["author"] | "";
        row.subtitle = item["body"] | "";
        row.value = item["meta"] | "";
        if (!row.title.empty() && !row.subtitle.empty()) rows.push_back(std::move(row));
      }
      collecting = doc["collecting"] | false;
      if (!collecting || (!advance && !rows.empty()) || (advance && rows.size() > baseline) || pass == 7) break;
      vTaskDelay(pdMS_TO_TICKS(700));
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (advance) {
      for (auto& row : rows) {
        const bool duplicate = std::any_of(comments_.begin(), comments_.end(), [&](const auto& old) {
          return old.title == row.title && old.subtitle == row.subtitle;
        });
        if (!duplicate) comments_.push_back(std::move(row));
      }
    } else {
      comments_ = std::move(rows);
    }
    commentsText_.clear();
    for (size_t i = 0; i < comments_.size(); ++i) {
      const auto& row = comments_[i];
      commentsText_ += std::to_string(i + 1) + ". " + row.title;
      if (!row.value.empty()) commentsText_ += "  " + row.value;
      commentsText_ += "\n" + row.subtitle + "\n\n";
    }
    status_ = comments_.empty() ? "暂无可见评论"
        : "已缓存 " + std::to_string(comments_.size()) + " 条评论"
          + (collecting ? " · 手机后台继续采集" : "");
    ++revision_;
    return true;
  }

  bool loadImage(int index) {
    std::string body;
    if (!request("GET", "/v2/xhs/image?index=" + std::to_string(index), body, 64u * 1024u) ||
        body.size() < 62 || body[0] != 'B' || body[1] != 'M') {
      return setFailure("笔记图片读取失败");
    }
    if (!M4NativeProviderIo::ensureParentDirs(imagePath_)) return setFailure("图片缓存目录不可用");
    const std::string part = imagePath_ + ".part";
    if (SdMan.exists(part.c_str())) SdMan.remove(part.c_str());
    FsFile file;
    if (!SdMan.openFileForWrite("SB-IMG", part.c_str(), file)) return setFailure("图片缓存写入失败");
    const int written = file.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    file.sync();
    file.close();
    if (written != static_cast<int>(body.size()) ||
        !M4NativeProviderIo::commitTempFile(part, imagePath_, body.size())) {
      return setFailure("图片缓存提交失败");
    }
    std::lock_guard<std::mutex> lock(mu_);
    imageIndex_ = std::max(0, std::min(std::max(0, imageCount_ - 1), index));
    status_ = "图片 " + std::to_string(imageIndex_ + 1) + "/" + std::to_string(imageCount_);
    ++revision_;
    return true;
  }

  bool setFailure(const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = message;
    ++revision_;
    return false;
  }

  M4xInstalledApp app_;
  std::string endpointPath_;
  mutable std::mutex mu_;
  std::vector<M4NativeUi::Row> apps_;
  std::vector<M4NativeUi::Row> feed_;
  std::vector<M4NativeUi::Row> comments_;
  std::string base_;
  std::string status_ = "正在连接手机…";
  std::string selectedKey_;
  std::string noteTitle_;
  std::string noteAuthor_;
  std::string noteBody_;
  std::string commentCount_;
  std::string likes_;
  std::string collects_;
  std::string timestamp_;
  std::string commentsText_;
  std::string imagePath_ = "/apps_data/com.m4screenbridge.client/cache/xhs-image.bmp";
  int imageCount_ = 0;
  int imageIndex_ = 0;
  uint32_t revision_ = 1;
  Job pending_ = Job::None;
  bool initialQueued_ = false;
  bool stop_ = false;
#if defined(ARDUINO_ARCH_ESP32)
  TaskHandle_t task_ = nullptr;
#endif
};

}  // namespace

std::unique_ptr<M4NativeUi::Controller> createScreenBridgeController(const M4xInstalledApp& app) {
  return std::make_unique<ScreenBridgeController>(app);
}

}  // namespace M4NativeAppControllers

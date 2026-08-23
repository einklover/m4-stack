#include "apps/native/M4NativeAppControllerFactory.h"
#include "apps/native/M4ScreenBridgeController.h"

#include "apps/providers/M4NativeLoadUi.h"
#include "apps/providers/M4NativeProviderDiscovery.h"
#include "apps/providers/M4NativeProviderExplore.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4ProviderShelfIndex.h"

#include <SDCardManager.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace M4NativeAppControllers {
namespace {

bool fieldAt(const std::string& line, int field, std::string& out) {
  out.clear();
  int cur = 0;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i != line.size() && line[i] != '\t') continue;
    if (cur == field) {
      out.assign(line, start, i - start);
      return true;
    }
    ++cur;
    start = i + 1;
  }
  return false;
}

bool readLineAt(const std::string& path, size_t target, const std::vector<uint32_t>& anchors,
                std::string& line) {
  line.clear();
  if (anchors.empty()) return false;
  const size_t slot = std::min(M4ProviderShelfIndex::anchorSlot(target), anchors.size() - 1);
  size_t row = M4ProviderShelfIndex::anchorRow(target);

  FsFile f;
  if (!SdMan.openFileForRead("NA-SHELF", path.c_str(), f)) return false;
  if (!f.seek(anchors[slot])) {
    f.close();
    return false;
  }

  uint8_t buf[512];
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (row == target) {
        if (c == '\n') {
          f.close();
          if (!line.empty() && line.back() == '\r') line.pop_back();
          return !line.empty();
        }
        // Leading NULs / controls (FatFS slack) are not part of the TSV row.
        if (line.empty() && static_cast<unsigned char>(c) < 0x20 && c != '\t') continue;
        if (line.size() >= 2048) {
          f.close();
          return false;
        }
        line.push_back(c);
      } else if (c == '\n') {
        ++row;
      }
    }
  }
  f.close();
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return row == target && !line.empty();
}

constexpr size_t kMaxShelfBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxShelfRows = 200000;
constexpr size_t kShelfPumpBytes = 4096;
constexpr uint32_t kShelfRevisionIntervalMs = 300;

class BaseController : public M4NativeUi::Controller {
 public:
  explicit BaseController(M4xInstalledApp app) : app_(std::move(app)) {}

  bool scalar(const std::string& key, std::string& out) const override {
    if (key == "app.name") out = app_.name;
    else if (key == "app.id") out = app_.id;
    else if (key == "app.version") out = app_.version;
    else if (key == "app.provider") out = app_.provider;
    else if (key == "runtime.status") out = "native";
    else {
      out.clear();
      return false;
    }
    return true;
  }

  M4NativeUi::ActionResult dispatch(const std::string& action,
                                    const M4NativeUi::ActionContext& ctx) override {
    (void)ctx;
    if (action == "system.back" || action == "system.close") return M4NativeUi::ActionResult::close();
    if (action.rfind("ui.go:", 0) == 0) {
      const std::string target = action.substr(6);
      if (!target.empty()) return M4NativeUi::ActionResult::navigate(target);
    }
    M4NativeUi::ActionResult r;
    r.kind = M4NativeUi::ActionKind::Error;
    r.error = action.empty() ? "empty_action" : "unsupported_action";
    return r;
  }

 protected:
  M4xInstalledApp app_;
};

class ProviderController final : public BaseController {
 public:
  explicit ProviderController(M4xInstalledApp app) : BaseController(std::move(app)) {
    root_ = std::string("/apps_data/") + app_.id;
    shelfRows_ = root_ + "/provider/shelf_rows.tsv";
    shelfIndexBuilder_.maxRows = kMaxShelfRows;
    beginShelfIndex();
    M4NativeProviderExplore::Category first{};
    if (M4NativeProviderExplore::at(app_.provider, 0, first)) {
      selectedCategoryKey_ = first.key;
      selectedCategoryTitle_ = first.title;
    }
  }

  ~ProviderController() override {
    if (shelfIndexFile_.isOpen()) shelfIndexFile_.close();
  }

  bool scalar(const std::string& key, std::string& out) const override {
    if (BaseController::scalar(key, out)) return true;

    if (key == "page.status") {
      syncDiscovery();
      const auto chapter = M4NativeProviderManager::progress();
      if (chapter.providerId == app_.provider && chapter.phase == M4NativeProvider::Phase::AuthRequired) {
        out = app_.provider == "weread" ? "登录已失效 · 请选择登录" : "登录已失效";
        return true;
      }

      // Do not start FreeRTOS discovery (TLS) as a side effect of resolving UI
      // text during render — that races the frame path and can starve the main
      // loop under QEMU open_eth. kickAutoDiscovery() runs from the activity loop.
      const auto d = M4NativeProviderDiscovery::snapshot();
      if (d.providerId == app_.provider && d.appId == app_.id) {
        M4NativeLoadUi::Snapshot load;
        load.scope = M4NativeLoadUi::Scope::Discovery;
        load.receivedBytes = d.receivedBytes;
        load.rows = d.rowCount;
        load.elapsedSeconds = d.startedMs && d.updatedMs >= d.startedMs ? (d.updatedMs - d.startedMs) / 1000u : 0;
        if (d.phase == M4NativeProviderDiscovery::Phase::Connecting) {
          load.stage = M4NativeLoadUi::Stage::Connecting;
          out = M4NativeLoadUi::title(load);
          return true;
        }
        if (d.phase == M4NativeProviderDiscovery::Phase::Receiving) {
          load.stage = M4NativeLoadUi::Stage::Receiving;
          out = M4NativeLoadUi::title(load) + " · " + M4NativeLoadUi::detail(load);
          return true;
        }
        if (d.phase == M4NativeProviderDiscovery::Phase::AuthRequired) {
          out = app_.provider == "weread" ? "登录后同步微信读书书架" : "需要登录后继续";
          return true;
        }
        if (d.phase == M4NativeProviderDiscovery::Phase::Error && shelfCount_ == 0) {
          // Surface the machine error so LAN/parse failures are diagnosable on-device
          // instead of a generic "network failed" (often not a network problem).
          if (d.error == "legado_endpoint_missing") {
            out = "未找到阅读服务 · 请打开连接设置";
          } else if (!d.error.empty() && d.error.size() <= 40) {
            out = std::string("失败 · ") + d.error;
          } else {
            out = "联网失败 · 请选择刷新重试";
          }
          return true;
        }
      }

      if (shelfIndexing_) {
        out = "正在整理书目…";
        return true;
      }
      if (shelfIndexFailed_) {
        out = "书目读取失败 · 刷新重试";
        return true;
      }
      if (shelfCount_ == 0) {
        out = (app_.provider == "weread" || app_.provider == "legado") ? "暂无书目" : "暂无热推";
      } else if (!selectedCategoryTitle_.empty()) {
        out = selectedCategoryTitle_ + " · " + std::to_string(shelfCount_) + " 本";
      } else {
        out = std::string("书架 · ") + std::to_string(shelfCount_) + " 本";
      }
      return true;
    }

    if (key == "page.heading") {
      if (!selectedCategoryTitle_.empty()) out = selectedCategoryTitle_ + "热推";
      else out = "书架";
      return true;
    }

    out.clear();
    return false;
  }

  size_t rowCount(const std::string& source) const override {
    syncDiscovery();
    if (source == "provider.categories") return M4NativeProviderExplore::count(app_.provider);
    if (source == "provider.books" || source == "provider.shelf" || source == "provider.recommend") {
      return shelfIndexReady_ ? shelfCount_ : 0;
    }
    return 0;
  }

  bool rowAt(const std::string& source, size_t index0, M4NativeUi::Row& out) const override {
    syncDiscovery();
    out = {};
    if (source == "provider.categories") {
      M4NativeProviderExplore::Category category{};
      if (!M4NativeProviderExplore::at(app_.provider, index0, category)) return false;
      out.key = category.key;
      out.title = category.title;
      out.subtitle = category.subtitle;
      return true;
    }

    if (!shelfIndexReady_ ||
        (source != "provider.books" && source != "provider.shelf" && source != "provider.recommend") ||
        index0 >= shelfCount_) return false;
    std::string line;
    if (!readLineAt(shelfRows_, index0, shelfAnchors_, line)) return false;
    std::string rawKey;
    fieldAt(line, 0, rawKey);
    if (!M4ContentProvider::sanitizeId(rawKey, M4ContentProvider::kMaxBookIdLen, out.key)) {
      return false;
    }
    fieldAt(line, 1, out.title);
    fieldAt(line, 2, out.subtitle);
    fieldAt(line, 3, out.value);
    if (out.title.empty()) out.title = out.key;
    if (!out.value.empty()) out.value += "%";
    return !out.key.empty();
  }

  M4NativeUi::ActionResult dispatch(const std::string& action,
                                    const M4NativeUi::ActionContext& ctx) override {
    if (action == "system.back" || action == "system.close") return M4NativeUi::ActionResult::close();

    if (action == "provider.openBook" || action == "provider.openSelected") {
      std::string bookId;
      if (!M4ContentProvider::sanitizeId(ctx.rowKey, M4ContentProvider::kMaxBookIdLen, bookId)) {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = ctx.rowKey.empty() ? "book_not_selected" : "provider_bad_route";
        return r;
      }
      std::string title;
      M4NativeUi::Row row;
      if (ctx.index0 >= 0 && rowAt(ctx.source.empty() ? "provider.recommend" : ctx.source,
                                  static_cast<size_t>(ctx.index0), row)) title = row.title;
      (void)M4NativeProviderManager::ensureBook(app_.provider, bookId, app_.id, title);
      return M4NativeUi::ActionResult::openProviderBook(
          M4ContentProvider::makeHistoryUri(app_.provider.c_str(), bookId.c_str()));
    }

    if (action == "provider.login") {
      M4NativeUi::ActionResult r;
      r.kind = M4NativeUi::ActionKind::OpenLogin;
      r.payload = app_.provider;
      return r;
    }

    if (action == "provider.endpoint") {
      if (app_.provider != "legado") {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "endpoint_not_supported";
        return r;
      }
      return M4NativeUi::ActionResult::openEndpoint();
    }

    if (action == "provider.selectCategory") {
      M4NativeProviderExplore::Category category{};
      if (!M4NativeProviderExplore::find(app_.provider, ctx.rowKey, category)) {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "bad_category";
        return r;
      }
      if (M4NativeProviderDiscovery::busy()) {
        const auto active = M4NativeProviderDiscovery::snapshot();
        if (active.providerId == app_.provider && active.appId == app_.id) {
          return M4NativeUi::ActionResult::repaint();
        }
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "provider_busy";
        return r;
      }
      selectedCategoryKey_ = category.key;
      selectedCategoryTitle_ = category.title;
      if (!M4NativeProviderDiscovery::startCategory(app_.provider, app_.id, selectedCategoryKey_)) {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "discovery_start_failed";
        return r;
      }
      autoDiscoveryAttempted_ = true;
      return M4NativeUi::ActionResult::repaint();
    }

    if (action == "provider.refresh") {
      if (M4NativeProviderDiscovery::busy()) {
        const auto active = M4NativeProviderDiscovery::snapshot();
        if (active.providerId == app_.provider && active.appId == app_.id) {
          autoDiscoveryAttempted_ = true;
          return M4NativeUi::ActionResult::repaint();
        }
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "provider_busy";
        return r;
      }
      bool started = false;
      if (!selectedCategoryKey_.empty()) started = M4NativeProviderDiscovery::startCategory(app_.provider, app_.id, selectedCategoryKey_);
      else started = M4NativeProviderDiscovery::startDefault(app_.provider, app_.id);
      if (started || M4NativeProviderDiscovery::busy()) {
        autoDiscoveryAttempted_ = true;
        return M4NativeUi::ActionResult::repaint();
      }
      M4NativeUi::ActionResult r;
      r.kind = M4NativeUi::ActionKind::Error;
      r.error = "discovery_start_failed";
      return r;
    }

    return BaseController::dispatch(action, ctx);
  }

  uint32_t revision() const override {
    const auto d = M4NativeProviderDiscovery::snapshot();
    const auto p = M4NativeProviderManager::progress();
    uint32_t rev = revision_;
    if (d.providerId == app_.provider && d.appId == app_.id) rev ^= (static_cast<uint32_t>(d.phase) << 28) ^ ((d.updatedMs / 1000u) & 0x0FFFFFFFu);
    if (p.providerId == app_.provider) rev ^= (static_cast<uint32_t>(p.phase) << 24) ^ (p.updatedMs & 0x00FFFFFFu);
    return rev;
  }

  void pollAsync() override {
    syncDiscovery();
    if (shelfIndexing_) {
      pumpShelfIndex();
      return;
    }
    if (!shelfIndexReady_) {
      beginShelfIndex();
      if (shelfIndexing_) pumpShelfIndex();
      if (!shelfIndexReady_) return;
    }
    if (shelfCount_ != 0 || autoDiscoveryAttempted_ || M4NativeProviderDiscovery::busy()) return;
    // One-shot auto fill for public discovery providers (fanqie/jjwxc) and
    // account shelves (weread/legado). Kept out of scalar()/render().
    autoDiscoveryAttempted_ = true;
    if (!selectedCategoryKey_.empty()) {
      (void)M4NativeProviderDiscovery::startCategory(app_.provider, app_.id, selectedCategoryKey_);
    } else {
      (void)M4NativeProviderDiscovery::startDefault(app_.provider, app_.id);
    }
  }

 private:
  void beginShelfIndex() const {
    if (shelfIndexing_) return;
    if (shelfIndexFile_.isOpen()) shelfIndexFile_.close();
    shelfIndexBuilder_ = {};
    shelfIndexBuilder_.maxRows = kMaxShelfRows;
    shelfAnchors_.clear();
    shelfCount_ = 0;
    shelfIndexFailed_ = false;
    shelfIndexError_.clear();
    shelfIndexReady_ = false;

    if (!SdMan.exists(shelfRows_.c_str())) {
      shelfIndexReady_ = true;
      return;
    }
    if (!SdMan.openFileForRead("NA-SHELF", shelfRows_.c_str(), shelfIndexFile_)) {
      shelfIndexFailed_ = true;
      shelfIndexReady_ = true;
      shelfIndexError_ = "shelf_open_failed";
      ++revision_;
      return;
    }
    if (shelfIndexFile_.fileSize() == 0) {
      shelfIndexFile_.close();
      shelfIndexReady_ = true;
      return;
    }
    if (shelfIndexFile_.fileSize() > kMaxShelfBytes) {
      shelfIndexFile_.close();
      shelfIndexFailed_ = true;
      shelfIndexReady_ = true;
      shelfIndexError_ = "shelf_too_large";
      ++revision_;
      return;
    }
    shelfIndexing_ = true;
    shelfIndexLastRevisionMs_ = millis();
    ++revision_;
  }

  void finishShelfIndex(bool failed) const {
    if (shelfIndexFile_.isOpen()) shelfIndexFile_.close();
    shelfIndexing_ = false;
    shelfIndexReady_ = true;
    const size_t count = shelfIndexBuilder_.finish();
    if (failed || shelfIndexBuilder_.overflow || count > kMaxShelfRows) {
      shelfCount_ = 0;
      shelfAnchors_.clear();
      shelfIndexFailed_ = true;
      shelfIndexError_ = shelfIndexBuilder_.overflow || count > kMaxShelfRows
                             ? "shelf_too_many_rows"
                             : "shelf_read_failed";
    } else {
      shelfCount_ = count;
      shelfAnchors_ = std::move(shelfIndexBuilder_.anchors);
    }
    ++revision_;
  }

  void pumpShelfIndex() const {
    if (!shelfIndexing_) return;
    uint8_t buf[kShelfPumpBytes];
    const int n = shelfIndexFile_.read(buf, sizeof(buf));
    if (n <= 0) {
      finishShelfIndex(true);
      return;
    }
    shelfIndexBuilder_.feed(buf, static_cast<size_t>(n));
    if (shelfIndexBuilder_.overflow) {
      finishShelfIndex(true);
      return;
    }
    if (!shelfIndexFile_.available()) {
      finishShelfIndex(false);
      return;
    }
    const uint32_t now = millis();
    if (now - shelfIndexLastRevisionMs_ >= kShelfRevisionIntervalMs) {
      shelfIndexLastRevisionMs_ = now;
      ++revision_;
    }
  }

  void syncDiscovery() const {
    const auto d = M4NativeProviderDiscovery::snapshot();
    if (d.providerId != app_.provider || d.appId != app_.id) return;
    if (d.phase == M4NativeProviderDiscovery::Phase::Ready && d.updatedMs != discoveryAppliedMs_) {
      discoveryAppliedMs_ = d.updatedMs;
      beginShelfIndex();
    }
  }

  std::string root_;
  std::string shelfRows_;
  mutable FsFile shelfIndexFile_;
  mutable M4ProviderShelfIndex::Builder shelfIndexBuilder_;
  mutable size_t shelfCount_ = 0;
  mutable std::vector<uint32_t> shelfAnchors_;
  mutable uint32_t discoveryAppliedMs_ = 0;
  mutable uint32_t shelfIndexLastRevisionMs_ = 0;
  mutable uint32_t revision_ = 0;
  mutable bool shelfIndexing_ = false;
  mutable bool shelfIndexReady_ = false;
  mutable bool shelfIndexFailed_ = false;
  mutable std::string shelfIndexError_;
  bool autoDiscoveryAttempted_ = false;
  std::string selectedCategoryKey_;
  std::string selectedCategoryTitle_;
};

}  // namespace

std::unique_ptr<M4NativeUi::Controller> create(const M4xInstalledApp& app) {
  if (app.runtime == M4xRuntimeKind::Native && app.provider == "screenbridge") {
    return createScreenBridgeController(app);
  }
  if (app.runtime == M4xRuntimeKind::Native && !app.provider.empty() && M4NativeProviderManager::supports(app.provider)) {
    return std::make_unique<ProviderController>(app);
  }
  return std::make_unique<BaseController>(app);
}

}  // namespace M4NativeAppControllers

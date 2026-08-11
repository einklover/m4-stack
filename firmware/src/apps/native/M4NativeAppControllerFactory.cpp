#include "apps/native/M4NativeAppControllerFactory.h"

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4NativeLoadUi.h"
#include "apps/providers/M4NativeProviderDiscovery.h"
#include "apps/providers/M4NativeProviderExplore.h"
#include "apps/providers/M4NativeProviderIo.h"
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

size_t buildShelfIndex(const std::string& path, std::vector<uint32_t>& anchors) {
  anchors.clear();
  FsFile f;
  if (!SdMan.openFileForRead("NA-SHELF", path.c_str(), f)) return 0;
  M4ProviderShelfIndex::Builder index;
  uint8_t buf[1024];
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    index.feed(buf, static_cast<size_t>(n));
  }
  f.close();
  anchors = std::move(index.anchors);
  return index.finish();
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

class ShelfSink final : public M4xJsonStream::Sink {
 public:
  ~ShelfSink() override {
    if (open_) f_.close();
  }

  bool open(const std::string& path) {
    M4NativeProviderIo::ensureParentDirs(path);
    if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
    open_ = SdMan.openFileForWrite("NA-SHELF", path.c_str(), f_);
    return open_;
  }

  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    return f_.write(data, len) == static_cast<int>(len);
  }

 private:
  FsFile f_;
  bool open_ = false;
};

bool projectLegacyShelf(const M4xInstalledApp& app, const std::string& rowsPath) {
  const std::string root = std::string("/apps_data/") + app.id;
  std::string source = root + "/shelf_cache.json";
  if (!SdMan.exists(source.c_str())) source = root + "/shelf.json";
  if (!SdMan.exists(source.c_str())) return false;
  FsFile in;
  if (!SdMan.openFileForRead("NA-SHELF", source.c_str(), in) || in.fileSize() > 4u * 1024u * 1024u) {
    if (in.isOpen()) in.close();
    return false;
  }
  ShelfSink sink;
  if (!sink.open(rowsPath)) {
    in.close();
    return false;
  }
  M4xJsonStream::RecordExtractor rows({"books"}, {"bookId", "title", "author", "progress"}, sink, 4096);
  uint8_t buf[2048];
  bool ok = true;
  while (in.available()) {
    const int n = in.read(buf, sizeof(buf));
    if (n <= 0) {
      ok = false;
      break;
    }
    if (!rows.feed(buf, static_cast<size_t>(n))) {
      ok = false;
      break;
    }
  }
  in.close();
  return ok && rows.finish() && rows.recordCount() > 0;
}

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
    shelfCount_ = buildShelfIndex(shelfRows_, shelfAnchors_);
    if (shelfCount_ == 0) {
      (void)projectLegacyShelf(app_, shelfRows_);
      shelfCount_ = buildShelfIndex(shelfRows_, shelfAnchors_);
    }
    M4NativeProviderExplore::Category first{};
    if (M4NativeProviderExplore::at(app_.provider, 0, first)) {
      selectedCategoryKey_ = first.key;
      selectedCategoryTitle_ = first.title;
    }
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

      if (shelfCount_ == 0 && !autoDiscoveryAttempted_ && !M4NativeProviderDiscovery::busy()) {
        autoDiscoveryAttempted_ = true;
        if (!selectedCategoryKey_.empty()) {
          (void)M4NativeProviderDiscovery::startCategory(app_.provider, app_.id, selectedCategoryKey_);
        } else {
          (void)M4NativeProviderDiscovery::startDefault(app_.provider, app_.id);
        }
      }

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
            out = "未找到阅读服务 · 请用手机打开传书页后刷新";
          } else if (!d.error.empty() && d.error.size() <= 40) {
            out = std::string("失败 · ") + d.error;
          } else {
            out = "联网失败 · 请选择刷新重试";
          }
          return true;
        }
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
    if (source == "provider.books" || source == "provider.shelf" || source == "provider.recommend") return shelfCount_;
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

    if ((source != "provider.books" && source != "provider.shelf" && source != "provider.recommend") || index0 >= shelfCount_) return false;
    std::string line;
    if (!readLineAt(shelfRows_, index0, shelfAnchors_, line)) return false;
    fieldAt(line, 0, out.key);
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
      if (ctx.rowKey.empty()) {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "book_not_selected";
        return r;
      }
      std::string title;
      M4NativeUi::Row row;
      if (ctx.index0 >= 0 && rowAt(ctx.source.empty() ? "provider.recommend" : ctx.source,
                                  static_cast<size_t>(ctx.index0), row)) title = row.title;
      (void)M4NativeProviderManager::ensureBook(app_.provider, ctx.rowKey, app_.id, title);
      return M4NativeUi::ActionResult::openProviderBook(M4ContentProvider::makeHistoryUri(app_.provider.c_str(), ctx.rowKey.c_str()));
    }

    if (action == "provider.login") {
      M4NativeUi::ActionResult r;
      r.kind = M4NativeUi::ActionKind::OpenLogin;
      r.payload = app_.provider;
      return r;
    }

    if (action == "provider.selectCategory") {
      M4NativeProviderExplore::Category category{};
      if (!M4NativeProviderExplore::find(app_.provider, ctx.rowKey, category)) {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "bad_category";
        return r;
      }
      if (M4NativeProviderDiscovery::busy()) return M4NativeUi::ActionResult::repaint();
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
    uint32_t rev = 0;
    if (d.providerId == app_.provider && d.appId == app_.id) rev ^= (static_cast<uint32_t>(d.phase) << 28) ^ ((d.updatedMs / 1000u) & 0x0FFFFFFFu);
    if (p.providerId == app_.provider) rev ^= (static_cast<uint32_t>(p.phase) << 24) ^ (p.updatedMs & 0x00FFFFFFu);
    return rev;
  }

 private:
  void syncDiscovery() const {
    const auto d = M4NativeProviderDiscovery::snapshot();
    if (d.providerId != app_.provider || d.appId != app_.id) return;
    if (d.phase == M4NativeProviderDiscovery::Phase::Ready && d.updatedMs != discoveryAppliedMs_) {
      shelfCount_ = buildShelfIndex(shelfRows_, shelfAnchors_);
      discoveryAppliedMs_ = d.updatedMs;
    }
  }

  std::string root_;
  std::string shelfRows_;
  mutable size_t shelfCount_ = 0;
  mutable std::vector<uint32_t> shelfAnchors_;
  mutable uint32_t discoveryAppliedMs_ = 0;
  mutable bool autoDiscoveryAttempted_ = false;
  std::string selectedCategoryKey_;
  std::string selectedCategoryTitle_;
};

}  // namespace

std::unique_ptr<M4NativeUi::Controller> create(const M4xInstalledApp& app) {
  if (app.runtime == M4xRuntimeKind::Native && !app.provider.empty() && M4NativeProviderManager::supports(app.provider)) {
    return std::make_unique<ProviderController>(app);
  }
  return std::make_unique<BaseController>(app);
}

}  // namespace M4NativeAppControllers

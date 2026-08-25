#include "NativeProviderBookActivity.h"
#include "NativeProviderLoginActivity.h"

#include "MappedInputManager.h"

#include "activities/reader/TxtReaderActivity.h"
#include "activities/reader/TxtReaderChapterSelectionActivity.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/M4PluginReaderSession.h"
#include "apps/providers/M4NativeLoadUi.h"
#include "apps/providers/M4NativeProviderBookDetail.h"
#include "apps/providers/M4NativeProviderBookDetailAsync.h"
#include "apps/providers/M4NativeProviderCatalog.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "RecentBooksStore.h"
#include "util/M4ErrorScreen.h"
#include "util/M4ProviderCoverCache.h"
#include "util/M4PluginReaderBridge.h"
#include "util/M4PluginTocList.h"
#include "util/M4UiText.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>
#include <Txt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kChapterLoadingTimeoutMs = 45u * 1000u;

std::string chapterErrorText(const std::string& code) {
  if (code == "http_request_failed" || code.rfind("http_ESP_ERR", 0) == 0) {
    return "网络请求失败（时间不准时会自动校时，请再试）";
  }
  if (code == "http_begin_failed") return "无法建立网络连接";
  if (code == "wifi_not_connected") return "Wi-Fi 未连接";
  if (code == "http_401" || code == "http_403") return "登录已失效";
  if (code == "login_required") return "登录已失效，请重新登录";
  if (code == "http_404") return "章节不存在";
  if (code == "http_429") return "请求过于频繁";
  if (code == "response_too_large") return "章节内容过大";
  if (code == "tls_internal_oom") return "TLS 临时内存不足，请返回后重试";
  if (code == "heap_corrupt") return "检测到内存异常，请返回后重试";
  if (code == "body_stream_failed") return "正文接收失败";
  if (code == "empty_content" || code == "psvts_not_found") return "未获取到有效正文，请重试";
  if (code == "http_2xx_empty") return "内容源返回空数据，请重试";
  if (code == "shard_json" || code == "shard_bad_header" || code == "shard_md5") {
    return "正文分片校验失败，请重试";
  }
  if (code == "shard_oom") return "正文处理内存不足，请返回后重试";
  if (code == "shard_io") return "正文分片读写失败";
  if (code == "sink_failed" || code == "sink_write_failed" || code == "sd_open_failed") {
    return "写入 SD 卡失败（请检查存储空间后重试）";
  }
  if (code == "cache_commit_failed") return "缓存提交失败，已清理并重试";
  if (code == "catalog_resolve") return "目录章节信息无效";
  if (code == "provider_not_supported") return "内容源不支持此章节";
  if (code == "cancelled") return "加载已取消";
  if (code == "chapter_timeout") return "正文请求超时，请重试";
  return code.empty() ? "未知错误" : code;
}

std::string detailErrorText(const std::string& code) {
  if (code == "login_required" || code == "http_401" || code == "http_403") {
    return "简介需要登录，可继续阅读";
  }
  if (code == "detail_task_create" || code == "detail_busy") {
    return "简介任务繁忙，可稍后重试";
  }
  if (code == "detail_json" || code == "detail_empty") {
    return "简介数据无效，可继续阅读";
  }
  if (code == "http_request_failed" || code == "http_begin_failed" || code == "detail_http") {
    return "简介请求失败，可继续阅读";
  }
  if (code == "cancelled") return "简介加载已取消";
  return "简介加载失败，可继续阅读";
}

std::string catalogErrorText(const std::string& code) {
  if (code == "login_required" || code == "http_401" || code == "http_403") return "需要登录后读取目录";
  if (code == "http_request_failed" || code == "catalog_http") return "目录网络请求失败";
  if (code == "http_404") return "未找到书籍目录";
  if (code == "http_429") return "请求过于频繁";
  if (code == "http_2xx_empty") return "目录源返回空数据，请重试";
  if (code == "catalog_empty") return "目录为空";
  // Open/commit failures and mid-write FatFS/SPI contention all present as
  // "can't persist the TOC". Wording steers users toward free space + retry.
  if (code == "catalog_commit_failed" || code == "sd_open_failed") {
    return "目录写入 SD 卡失败（请检查存储空间后重试）";
  }
  if (code == "sink_failed" || code == "sink_write_failed") {
    return "目录写入中断（存储繁忙，请再试）";
  }
  if (code == "catalog_register_failed") return "目录校验失败";
  if (code == "json_path_not_found") return "目录接口格式已变化";
  if (code == "json_token_too_large") return "目录条目过大";
  if (code == "json_too_many_records") return "目录章节过多";
  if (code == "json_syntax" || code == "json_truncated") return "目录数据解析失败";
  if (code == "book_locator_missing") return "书源定位丢失，请刷新书架";
  if (code == "legado_shelf_stale") return "手机端书架已变化，请在开源阅读 App 刷新书架后重试";
  if (code == "legado_endpoint_missing") return "未找到开源阅读服务，请先用手机打开本机传书页";
  if (code == "response_too_large") {
    return "目录数据过大（章节特别多，请更新固件或换较短书试）";
  }
  if (code == "cancelled") return "目录加载已取消";
  return code.empty() ? "目录加载失败" : code;
}

const char* providerDisplayName(const std::string& id) {
  if (id == "jjwxc") return "晋江文学城";
  if (id == "fanqie") return "番茄小说";
  if (id == "weread") return "微信读书";
  if (id == "legado") return "开源阅读";
  return "在线书源";
}

bool isAuthError(const std::string& code) {
  return code == "login_required" || code == "http_401" || code == "http_403";
}

M4NativeLoadUi::Stage catalogStage(M4NativeProviderCatalog::Phase phase) {
  switch (phase) {
    case M4NativeProviderCatalog::Phase::Connecting: return M4NativeLoadUi::Stage::Connecting;
    case M4NativeProviderCatalog::Phase::Receiving: return M4NativeLoadUi::Stage::Receiving;
    case M4NativeProviderCatalog::Phase::Registering: return M4NativeLoadUi::Stage::Processing;
    case M4NativeProviderCatalog::Phase::Ready: return M4NativeLoadUi::Stage::Ready;
    case M4NativeProviderCatalog::Phase::AuthRequired: return M4NativeLoadUi::Stage::AuthRequired;
    case M4NativeProviderCatalog::Phase::Error: return M4NativeLoadUi::Stage::Error;
    default: return M4NativeLoadUi::Stage::Preparing;
  }
}

M4NativeLoadUi::Stage chapterStage(M4NativeProvider::Phase phase) {
  switch (phase) {
    case M4NativeProvider::Phase::Resolving: return M4NativeLoadUi::Stage::Resolving;
    case M4NativeProvider::Phase::Connecting: return M4NativeLoadUi::Stage::Connecting;
    case M4NativeProvider::Phase::Receiving: return M4NativeLoadUi::Stage::Receiving;
    case M4NativeProvider::Phase::Decoding: return M4NativeLoadUi::Stage::Processing;
    case M4NativeProvider::Phase::Writing: return M4NativeLoadUi::Stage::Writing;
    case M4NativeProvider::Phase::Ready: return M4NativeLoadUi::Stage::Ready;
    case M4NativeProvider::Phase::AuthRequired: return M4NativeLoadUi::Stage::AuthRequired;
    case M4NativeProvider::Phase::Error: return M4NativeLoadUi::Stage::Error;
    case M4NativeProvider::Phase::Cancelled: return M4NativeLoadUi::Stage::Cancelled;
    default: return M4NativeLoadUi::Stage::Preparing;
  }
}

void appendMeta(std::string& out, const std::string& value) {
  if (value.empty()) return;
  if (!out.empty()) out += " · ";
  out += value;
}

std::string updateRecentProviderMetadata(const std::string& providerId, const std::string& bookId,
                                         const M4NovelProvider::BookDetail& detail) {
  const std::string uri = M4ContentProvider::makeHistoryUri(providerId.c_str(), bookId.c_str());
  if (uri.empty()) return {};
  const auto metrics = UITheme::getInstance().getMetrics();
  const auto cover = M4ProviderCoverCache::acquireProviderCover(
      M4ProviderCoverCache::requestFor(providerId, bookId, detail, metrics.homeCoverWidth,
                                       metrics.homeCoverThumbHeight));
  RECENT_BOOKS.updateProviderBook(uri, detail.title, detail.author, cover.coverBmpPath);
  return cover.coverBmpPath;
}

std::string displayWordCount(const std::string& raw) {
  if (raw.empty()) return {};
  bool digitsOnly = true;
  for (unsigned char c : raw) {
    if (!std::isdigit(c)) {
      digitsOnly = false;
      break;
    }
  }
  if (!digitsOnly) return raw;
  char* end = nullptr;
  const unsigned long long n = std::strtoull(raw.c_str(), &end, 10);
  if (!end || *end != '\0') return raw;
  if (n >= 10000ULL) {
    char buf[40];
    const unsigned long long tenths = (n + 500ULL) / 1000ULL;
    std::snprintf(buf, sizeof(buf), "%llu.%llu万字", tenths / 10ULL, tenths % 10ULL);
    return buf;
  }
  return raw + "字";
}

}  // namespace

NativeProviderBookActivity::NativeProviderBookActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::string providerId,
    std::string bookId, std::string appId, std::string title, std::string author,
    const std::function<void()>& onExitBook, bool autoStartReading, int autoOpenIndex)
    : ActivityWithSubactivity("NativeProviderBook", renderer, mappedInput),
      providerId_(std::move(providerId)),
      bookId_(std::move(bookId)),
      appId_(std::move(appId)),
      title_(std::move(title)),
      author_(std::move(author)),
      onExitBook_(onExitBook),
      autoStartReading_(autoStartReading),
      autoOpenIndex_(autoOpenIndex) {}

void NativeProviderBookActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  state_ = State::Detail;
  error_.clear();
  appDataRoot_ = std::string("/apps_data/") + appId_;
  // Local/persisted catalog discovery is cheap and does not start network I/O.
  // A missing catalog is intentionally not an error on the detail page.
  if (!prepareCatalog()) error_.clear();
  if (autoStartReading_) {
    // History / TOC-handoff: skip the detail page so chapter switch has an
    // owner without bouncing the user through the book card.
    if (titles_) {
      startReading();
    } else if (!startCatalogBootstrap(PendingCatalogAction::StartReading)) {
      loadBookDetail();
    }
    return;
  }
  loadBookDetail();
}

void NativeProviderBookActivity::onExit() {
  catalogStartPending_ = false;
  chapterStartPending_ = false;
  if (state_ == State::CatalogLoading) M4NativeProviderCatalog::cancel();
  cancelDetailLoading();
  ActivityWithSubactivity::onExit();
  titles_.reset();
}

bool NativeProviderBookActivity::prepareCatalog() {
  if (!M4NativeProviderManager::ensureBook(providerId_, bookId_, appId_, title_)) {
    error_ = "目录尚未准备好";
    return false;
  }
  appDataRoot_ = M4NativeProviderManager::appDataRootFor(providerId_, bookId_);
  M4ContentProvider::ChapterCatalogSpec catalog;
  if (!M4ContentProviderSession::catalogFor(providerId_, bookId_, 0, catalog) || catalog.fileRelPath.empty()) {
    error_ = "目录尚未准备好";
    return false;
  }
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(appDataRoot_, catalog.fileRelPath.c_str(), abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    error_ = "目录路径无效";
    return false;
  }
  titles_ = M4PluginTocList::openPagedFileRows(abs, catalog);
  if (!titles_ || titles_->rowCount() == 0) {
    error_ = "目录为空";
    return false;
  }
  chapterCount_ = static_cast<int>(titles_->rowCount());
  currentIndex_ = std::max(0, std::min(currentIndex_, chapterCount_ - 1));
  error_.clear();
  return true;
}

bool NativeProviderBookActivity::startCatalogBootstrap(PendingCatalogAction action) {
  catalogStartPending_ = false;
  titles_.reset();
  chapterCount_ = 0;
  loadingIndex_ = -1;
  error_.clear();
  lastCatalogPaintMs_ = 0;
  lastCatalogSignature_.clear();
  pendingCatalogAction_ = action;
  state_ = State::CatalogLoading;

  const auto existing = M4NativeProviderCatalog::snapshot();
  if (M4NativeProviderCatalog::busy()) {
    if (existing.providerId == providerId_ && existing.bookId == bookId_ && existing.appId == appId_) {
      renderCatalogLoading(true);
      return true;
    }
    error_ = "另一本书的目录正在加载";
    return false;
  }
  // Paint first and let the FAST_REFRESH finish. A small JJ catalog downloads
  // in ~1s; starting HTTPS while the panel still owns the shared SPI bus is
  // the usual catalog_commit_failed after a successful parse.
  renderCatalogLoading(true);
  // Defer the task start without blocking the activity loop. This lets Back
  // cancel the handoff while the first FAST frame settles on the panel.
  catalogStartAtMs_ = millis() + 600u;
  catalogStartPending_ = true;
  return true;
}

void NativeProviderBookActivity::continueAfterCatalogReady() {
  const PendingCatalogAction action = pendingCatalogAction_;
  pendingCatalogAction_ = PendingCatalogAction::None;
  if (action == PendingCatalogAction::StartReading) {
    startReading();
  } else {
    openToc();
  }
}

void NativeProviderBookActivity::loadBookDetail() {
  if (detailAttempted_) {
    renderDetail();
    return;
  }
  detailAttempted_ = true;

  M4NativeProviderBookDetail::Request req;
  req.providerId = providerId_;
  req.appId = appId_;
  req.bookId = bookId_;
  req.title = title_;
  req.author = author_;
  req.maxBytes = 96u * 1024u;
  detail_ = M4NativeProviderBookDetail::seed(req);
  detailError_.clear();
  const std::string coverPath = updateRecentProviderMetadata(providerId_, bookId_, detail_);
  if (!coverPath.empty()) providerCoverBmpPath_ = coverPath;

  // Paint the immediately available discovery/history model first (FAST only).
  // Legado detail is local-only (shelf row + seed) and must not block on a
  // whole-shelf HTTP refetch or endpoint probe when the phone is unreachable.
  detailLoading_ = true;
  renderDetail();
  if (!M4NativeProviderBookDetailAsync::start(req)) {
    detailLoading_ = false;
    detailError_ = "detail_busy";
    renderDetail();
  }
}

void NativeProviderBookActivity::pollDetailLoading() {
  if (!detailLoading_) return;
  const auto snap = M4NativeProviderBookDetailAsync::snapshot();
  if (snap.providerId != providerId_ || snap.appId != appId_ || snap.bookId != bookId_) return;
  if (snap.phase == M4NativeProviderBookDetailAsync::Phase::Idle ||
      snap.phase == M4NativeProviderBookDetailAsync::Phase::Loading) {
    return;
  }

  detailLoading_ = false;
  if (snap.phase == M4NativeProviderBookDetailAsync::Phase::Ready && snap.result.ok) {
    detail_ = snap.result.detail;
    detailError_.clear();
    if (!detail_.title.empty()) title_ = detail_.title;
    if (!detail_.author.empty()) author_ = detail_.author;
    const std::string coverPath = updateRecentProviderMetadata(providerId_, bookId_, detail_);
    if (!coverPath.empty()) providerCoverBmpPath_ = coverPath;
  } else {
    detailError_ = snap.result.error.empty() ? "detail_http" : snap.result.error;
  }
  renderDetail();
}

void NativeProviderBookActivity::cancelDetailLoading() {
  if (!detailLoading_) return;
  M4NativeProviderBookDetailAsync::cancel();
  detailLoading_ = false;
}

void NativeProviderBookActivity::renderDetail() {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int contentBottom = h - metrics.buttonHintsHeight - 6;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, providerDisplayName(providerId_));

  const int pad = metrics.contentSidePadding + 10;
  const int textWidth = std::max(1, w - 2 * pad);
  int y = metrics.topPadding + metrics.headerHeight + 12;
  detailReadButtonTop_ = 0;
  detailReadButtonHeight_ = 0;

  const std::string displayTitle = !detail_.title.empty() ? detail_.title
                                   : (!title_.empty() ? title_ : std::string("在线书籍"));
  const auto titleLines = M4UiText::wrapLines(renderer, UI_12_FONT_ID, displayTitle.c_str(), textWidth, 2,
                                               EpdFontFamily::BOLD);
  const int titleStep = M4UiText::listLineHeight(renderer, UI_12_FONT_ID) + 5;
  if (titleLines.empty()) {
    M4UiText::draw(renderer, UI_12_FONT_ID, pad, y, displayTitle.c_str(), true, EpdFontFamily::BOLD);
    y += titleStep;
  } else {
    for (const auto& line : titleLines) {
      M4UiText::draw(renderer, UI_12_FONT_ID, pad, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += titleStep;
    }
  }
  y += 3;

  const std::string displayAuthor = !detail_.author.empty() ? detail_.author : author_;
  if (!displayAuthor.empty() && y < contentBottom) {
    const std::string clipped = M4UiText::truncated(renderer, UI_10_FONT_ID, displayAuthor.c_str(), textWidth);
    M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, clipped.c_str());
    y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 5;
  }

  std::string meta;
  appendMeta(meta, detail_.kind);
  appendMeta(meta, detail_.status);
  appendMeta(meta, displayWordCount(detail_.wordCount));
  if (meta.empty()) meta = std::string("来源 · ") + providerDisplayName(providerId_);
  if (y < contentBottom) {
    const std::string clipped = M4UiText::truncated(renderer, UI_10_FONT_ID, meta.c_str(), textWidth);
    M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, clipped.c_str());
    y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 7;
  }

  const auto history = M4ContentProviderSession::makeHistorySnapshot(providerId_, bookId_);
  const bool hasHistory = history.providerId == providerId_ && history.bookId == bookId_ &&
                          (history.chapterIndex0 > 0 || history.hasByteOffset || history.page0 > 0);
  std::string resume;
  if (hasHistory) {
    resume = std::string("上次阅读 · 第 ") + std::to_string(history.chapterIndex0 + 1) + " 章";
    if (titles_ && history.chapterIndex0 >= 0 && history.chapterIndex0 < chapterCount_) {
      const std::string chapterTitle = titleAt(history.chapterIndex0);
      if (!chapterTitle.empty()) resume += " · " + chapterTitle;
    }
  } else {
    resume = "尚未阅读 · 将从第一章开始";
  }
  if (y < contentBottom) {
    const std::string clipped = M4UiText::truncated(renderer, UI_10_FONT_ID, resume.c_str(), textWidth);
    M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, clipped.c_str(), true, EpdFontFamily::BOLD);
    y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 8;
  }

  // 69shuba-inspired primary action: make reading the strongest visual target,
  // while keeping the standard footer/physical Confirm action for consistency.
  if (y + 50 < contentBottom) {
    detailReadButtonTop_ = y;
    detailReadButtonHeight_ = 48;
    renderer.drawRoundedRect(pad, y, textWidth, detailReadButtonHeight_, 2, 7, true);
    const char* primary = hasHistory ? "继续阅读" : "开始阅读";
    M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, pad, y, textWidth, detailReadButtonHeight_,
                                primary, true, EpdFontFamily::BOLD, 12);
    y += detailReadButtonHeight_ + 10;
  }

  if (y + 26 < contentBottom) {
    renderer.drawLine(pad, y, w - pad, y, true);
    y += 10;
    M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, "最近更新", true, EpdFontFamily::BOLD);
    y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 4;

    int recentShown = 0;
    if (titles_ && chapterCount_ > 0) {
      for (int i = chapterCount_ - 1; i >= 0 && recentShown < 3 && y < contentBottom; --i) {
        const std::string chapterTitle = titleAt(i);
        if (chapterTitle.empty()) continue;
        const std::string row = std::to_string(i + 1) + "  " + chapterTitle;
        const std::string clipped = M4UiText::truncated(renderer, UI_10_FONT_ID, row.c_str(), textWidth);
        M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, clipped.c_str());
        y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 3;
        ++recentShown;
      }
    } else if (!detail_.lastChapter.empty() && y < contentBottom) {
      const std::string clipped = M4UiText::truncated(renderer, UI_10_FONT_ID, detail_.lastChapter.c_str(), textWidth);
      M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, clipped.c_str());
      y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 3;
      recentShown = 1;
    }
    if (recentShown == 0 && y < contentBottom) {
      const std::string state = titles_ && chapterCount_ > 0
                                    ? std::to_string(chapterCount_) + " 章"
                                    : "章节目录按需加载";
      M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, state.c_str());
      y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 3;
    }
  }

  if (y + 34 < contentBottom) {
    renderer.drawLine(pad, y, w - pad, y, true);
    y += 10;
    M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, "简介", true, EpdFontFamily::BOLD);
    y += M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 4;

    const int lineStep = M4UiText::listLineHeight(renderer, UI_10_FONT_ID) + 4;
    const int available = std::max(0, contentBottom - y);
    const int maxLines = std::max(1, std::min(5, available / std::max(1, lineStep)));
    if (!detail_.intro.empty()) {
      const auto introLines = M4UiText::wrapLines(renderer, UI_10_FONT_ID, detail_.intro.c_str(), textWidth, maxLines);
      for (const auto& line : introLines) {
        if (y >= contentBottom) break;
        M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, line.c_str());
        y += lineStep;
      }
    } else if (y < contentBottom) {
      const std::string placeholder = detailLoading_
                                          ? "正在获取作品简介…"
                                          : (detailError_.empty() ? "暂无可用简介" : detailErrorText(detailError_));
      M4UiText::draw(renderer, UI_10_FONT_ID, pad, y, placeholder.c_str());
    }
  }

  const char* primary = hasHistory ? "继续阅读" : "开始阅读";
  const auto labels = mappedInput.mapLabels("« 返回", primary, "章节", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void NativeProviderBookActivity::openToc() {
  cancelDetailLoading();
  pendingCatalogAction_ = PendingCatalogAction::OpenToc;
  if (!titles_ && !prepareCatalog()) {
    error_.clear();
    if (!startCatalogBootstrap(PendingCatalogAction::OpenToc)) {
      state_ = State::Error;
      if (error_.empty()) error_ = "目录尚未准备好";
      renderError();
    }
    return;
  }
  pendingCatalogAction_ = PendingCatalogAction::None;
  tocBackPending_ = false;
  tocSelectionPending_ = false;
  tocSelectedIndex_ = -1;
  state_ = State::Toc;
  auto source = titles_;
  auto loader = [source](int first, int count, std::vector<std::string>& pageTitles,
                         std::vector<uint8_t>& pagePresent) {
    return source->loadPage(first, count, pageTitles, pagePresent);
  };
  enterNewActivity(new TxtReaderChapterSelectionActivity(
      renderer, mappedInput, chapterCount_, std::move(loader), currentIndex_,
      [this]() {
        tocBackPending_ = true;
        requestExitSubActivity();
      },
      [this](int index0) {
        tocSelectedIndex_ = index0;
        tocSelectionPending_ = true;
        requestExitSubActivity();
      },
      title_.empty() ? std::string("目  录") : title_));
}

void NativeProviderBookActivity::startReading() {
  cancelDetailLoading();
  pendingCatalogAction_ = PendingCatalogAction::StartReading;
  if (!titles_ && !prepareCatalog()) {
    error_.clear();
    if (!startCatalogBootstrap(PendingCatalogAction::StartReading)) {
      state_ = State::Error;
      if (error_.empty()) error_ = "无法加载目录";
      renderError();
    }
    return;
  }
  pendingCatalogAction_ = PendingCatalogAction::None;

  int index0 = 0;
  pendingInitialByteOffset_ = 0;
  hasPendingInitialByteOffset_ = false;
  pendingInitialIndex_ = -1;
  if (autoOpenIndex_ >= 0 && autoOpenIndex_ < chapterCount_) {
    index0 = autoOpenIndex_;
  } else {
    const auto history = M4ContentProviderSession::makeHistorySnapshot(providerId_, bookId_);
    if (history.providerId == providerId_ && history.bookId == bookId_ && history.chapterIndex0 >= 0 &&
        history.chapterIndex0 < chapterCount_) {
      index0 = history.chapterIndex0;
      if (history.hasByteOffset) {
        pendingInitialByteOffset_ = history.byteOffset;
        hasPendingInitialByteOffset_ = true;
        pendingInitialIndex_ = index0;
      }
    }
  }
  requestChapter(index0, autoOpenIndex_ >= 0);
}

std::string NativeProviderBookActivity::titleAt(int index0) const {
  if (!titles_ || index0 < 0) return {};
  std::vector<std::string> t;
  std::vector<uint8_t> p;
  if (!titles_->loadPage(index0, 1, t, p) || t.empty() || p.empty() || !p[0]) return {};
  return t[0];
}

void NativeProviderBookActivity::requestChapter(int index0, bool fromToc) {
  if (index0 < 0 || index0 >= chapterCount_) return;
  currentIndex_ = index0;
  loadingIndex_ = index0;
  loadingFromToc_ = fromToc;
  loadingTitle_ = titleAt(index0);
  chapterLoadStartedAtMs_ = 0;
  error_.clear();
  lastLoadingSignature_.clear();
  lastLoadingPaintMs_ = 0;
  // Cached body: open immediately. A history/TOC switch must not start TLS
  // just to re-fetch a chapter already on SD (wifi-down getaddrinfo panic).
  if (openReadyReader(index0)) return;
  state_ = State::Loading;
  // Same constraint as catalog bootstrap: FAST_REFRESH owns the shared SPI
  // bus. Starting TLS/HTTPS while the panel is still refreshing aborts the
  // handshake as ESP_ERR_HTTP_CONNECT (~100ms, 0 bytes). Paint first.
  renderLoading(true);
  // Defer the queue operation so the activity remains able to handle Back
  // during the panel-settle window instead of sleeping on the UI task.
  chapterStartAtMs_ = millis() + 600u;
  chapterStartPending_ = true;
}

void NativeProviderBookActivity::openLogin() {
  if (providerId_ != "weread" && providerId_ != "jjwxc") {
    error_ = "此内容源不支持登录";
    state_ = State::Error;
    renderError();
    return;
  }
  if (appDataRoot_.empty()) appDataRoot_ = std::string("/apps_data/") + appId_;
  loginFinishedPending_ = false;
  loginSucceeded_ = false;
  state_ = State::Login;
  enterNewActivity(new NativeProviderLoginActivity(
      renderer, mappedInput, providerId_, appDataRoot_,
      [this](bool success) {
        loginSucceeded_ = success;
        loginFinishedPending_ = true;
        requestExitSubActivity();
      }));
}

bool NativeProviderBookActivity::openReadyReader(int index0) {
  const auto st = M4ContentProviderSession::chapterAt(providerId_, bookId_, index0);
  if (st.state != M4ContentProvider::ChapterReady::Ready || st.cacheRelPath.empty()) return false;
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(appDataRoot_, st.cacheRelPath.c_str(), abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    error_ = "章节缓存路径无效";
    return false;
  }
  auto txt = std::make_unique<Txt>(abs, "/.crosspoint");
  if (!txt->load() || !txt->isEncodingSupported() || txt->getFileSize() == 0) {
    error_ = "章节缓存不可读取";
    return false;
  }

  TxtReaderActivity::PluginSession sess;
  sess.active = true;
  sess.suppressRecentBooks = false;
  sess.suppressOpenEpubPath = true;
  sess.progressiveIndex = true;
  sess.bookId = bookId_;
  sess.chapterUid = st.chapterUid;
  sess.chapterIndex = index0;
  sess.providerId = providerId_;
  sess.appId = appId_;
  sess.providerAuthor = !detail_.author.empty() ? detail_.author : author_;
  sess.providerCoverBmpPath = providerCoverBmpPath_;
  sess.appDataRoot = appDataRoot_;
  sess.cacheRelPath = st.cacheRelPath;
  sess.progressKey = providerId_ + ":" + bookId_ + ":" + st.chapterUid;
  sess.titleOverride = titleAt(index0);
  sess.generation = M4PluginReaderSession::bumpGeneration();
  sess.providerManaged = true;
  if (hasPendingInitialByteOffset_ && pendingInitialIndex_ == index0) {
    sess.initialByteOffset = pendingInitialByteOffset_;
    sess.hasInitialByteOffset = true;
  }
  hasPendingInitialByteOffset_ = false;
  pendingInitialIndex_ = -1;

  readerBackPending_ = false;
  state_ = State::Reader;
  auto onReaderClose = [this]() {
    int requestedIndex = -1;
    if (subActivity) {
      auto* r = static_cast<TxtReaderActivity*>(subActivity.get());
      const auto p = r->pluginProgressSnapshot();
      if (p.valid && p.switchChapterIndex >= 0) requestedIndex = p.switchChapterIndex;
    }
    if (requestedIndex >= 0) {
      tocSelectedIndex_ = requestedIndex;
      tocSelectionPending_ = true;
    }
    readerBackPending_ = true;
    requestExitSubActivity();
  };
  enterNewActivity(new TxtReaderActivity(renderer, mappedInput, std::move(txt), onReaderClose, onReaderClose,
                                         std::move(sess)));
  return true;
}

void NativeProviderBookActivity::renderCatalogLoading(bool force) {
  const uint32_t now = millis();
  const auto c = M4NativeProviderCatalog::snapshot();
  const bool mine = c.providerId == providerId_ && c.bookId == bookId_ && c.appId == appId_;
  const std::string failureCode = mine ? c.error : std::string();
  const bool authRequired = isAuthError(failureCode) ||
                            (mine && c.phase == M4NativeProviderCatalog::Phase::AuthRequired);

  M4NativeLoadUi::Snapshot load;
  load.scope = M4NativeLoadUi::Scope::Catalog;
  load.stage = mine ? catalogStage(c.phase) : M4NativeLoadUi::Stage::Preparing;
  load.receivedBytes = mine ? c.receivedBytes : 0;
  load.rows = mine ? c.rowCount : 0;
  const uint32_t started = mine && c.startedMs ? c.startedMs : now;
  load.elapsedSeconds = (now - started) / 1000u;
  load.error = failureCode;
  const std::string phase = M4NativeLoadUi::title(load);
  const std::string detail = M4NativeLoadUi::detail(load);
  // Throttle key intentionally omits elapsedSeconds: the loading screen used to
  // FAST_REFRESH every second purely because the clock advanced, racing the SD
  // SPI bus while the catalog task was writing toc_rows (common root of
  // "目录写入 SD 卡失败" after a successful multi-hundred-KB download).
  const int phaseInt = mine ? static_cast<int>(c.phase) : -1;
  const std::string sig = std::to_string(phaseInt) + "|" + std::to_string(load.receivedBytes) + "|" +
                          std::to_string(load.rows) + "|" + failureCode +
                          (authRequired ? "|auth" : "");
  // Error/Auth paints immediately; dense mid-phase progress is coalesced so the
  // e-ink panel is not FAST_REFRESHed every second while SD is busy.
  const bool urgent = authRequired || (mine && c.phase == M4NativeProviderCatalog::Phase::Error);
  if (!force) {
    if (sig == lastCatalogSignature_) return;
    if (!urgent && now - lastCatalogPaintMs_ < 2500u) {
      const size_t bar = lastCatalogSignature_.find('|');
      const size_t bar2 = sig.find('|');
      if (bar != std::string::npos && bar2 != std::string::npos &&
          lastCatalogSignature_.compare(0, bar, sig, 0, bar2) == 0) {
        return;
      }
    }
  }
  lastCatalogSignature_ = sig;
  lastCatalogPaintMs_ = now;

  const bool failed = mine && c.phase == M4NativeProviderCatalog::Phase::Error;
  const char* primary = authRequired ? "登录" : (failed ? "重试" : "");
  const auto labels = mappedInput.mapLabels("« 返回", primary, "", "");

  // Full diagnostic surface on failure/auth so a phone photo is enough to triage.
  if (failed || authRequired || !failureCode.empty()) {
    auto snap = M4ErrorScreen::catalogFail(
        title_.empty() ? "在线阅读" : title_,
        (authRequired ? std::string("需要登录：") : std::string("原因：")) +
            catalogErrorText(failureCode),
        failureCode, providerId_, bookId_, appId_, load.receivedBytes, load.rows,
        load.elapsedSeconds, labels.btn1, labels.btn2);
    if (authRequired) snap.title = "需要登录";
    M4ErrorScreen::addKV(snap.diag, "phase: ", phase);
    M4ErrorScreen::addKV(snap.diag, "progress: ", detail);
    M4ErrorScreen::paint(renderer, snap, true);
    return;
  }

  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 title_.empty() ? "在线阅读" : title_.c_str());
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 185, phase.c_str(), true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, 245, detail.c_str());
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void NativeProviderBookActivity::renderLoading(bool force) {
  const uint32_t now = millis();
  const auto p = M4NativeProviderManager::progress();
  const auto st = M4ContentProviderSession::chapterAt(providerId_, bookId_, loadingIndex_);
  const bool mine = p.providerId == providerId_ && p.bookId == bookId_ && p.chapterIndex0 == loadingIndex_;
  std::string failureCode = st.error;
  if (failureCode.empty() && mine) failureCode = p.error;
  const bool authRequired = isAuthError(failureCode) ||
                            (mine && p.phase == M4NativeProvider::Phase::AuthRequired);

  M4NativeLoadUi::Snapshot load;
  load.scope = M4NativeLoadUi::Scope::Chapter;
  load.stage = mine ? chapterStage(p.phase) : M4NativeLoadUi::Stage::Preparing;
  load.receivedBytes = mine ? p.receivedBytes : 0;
  load.writtenBytes = mine ? p.writtenBytes : 0;
  load.percent = mine ? p.percent : st.pct;
  const uint32_t started = mine && p.startedMs ? p.startedMs : now;
  load.elapsedSeconds = (now - started) / 1000u;
  load.error = failureCode;
  if (authRequired) load.stage = M4NativeLoadUi::Stage::AuthRequired;
  const std::string phase = M4NativeLoadUi::title(load);
  const std::string detail = M4NativeLoadUi::detail(load);
  const std::string sig = phase + "|" + detail + "|" + std::to_string(load.percent) + "|" +
                          std::to_string(static_cast<int>(st.state)) + "|" + failureCode;
  if (!force && sig == lastLoadingSignature_ && now - lastLoadingPaintMs_ < 1000) return;
  lastLoadingSignature_ = sig;
  lastLoadingPaintMs_ = now;

  const bool chapterFailed =
      !failureCode.empty() || st.state == M4ContentProvider::ChapterReady::Error;
  const char* primary = authRequired ? "登录" : (chapterFailed ? "重试" : "");
  const auto labels = mappedInput.mapLabels("« 返回", primary, "", "");

  if (chapterFailed || authRequired) {
    auto snap = M4ErrorScreen::chapterFail(
        title_.empty() ? "在线阅读" : title_, loadingTitle_,
        (authRequired ? std::string("需要登录：") : std::string("失败原因：")) +
            chapterErrorText(failureCode),
        failureCode, providerId_, bookId_, loadingIndex_, load.receivedBytes, load.writtenBytes,
        load.percent, load.elapsedSeconds, labels.btn1, labels.btn2);
    if (authRequired) snap.title = "需要登录";
    M4ErrorScreen::addKV(snap.diag, "phase: ", phase);
    M4ErrorScreen::addKV(snap.diag, "progress: ", detail);
    M4ErrorScreen::paint(renderer, snap, true);
    return;
  }

  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 title_.empty() ? "在线阅读" : title_.c_str());
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 180, phase.c_str(), true, EpdFontFamily::BOLD);
  if (!loadingTitle_.empty()) M4UiText::drawCentered(renderer, UI_10_FONT_ID, 230, loadingTitle_.c_str());
  if (load.percent > 0 && load.percent < 100) {
    GUI.drawProgressBar(renderer, Rect{70, 285, renderer.getScreenWidth() - 140, 12},
                        static_cast<size_t>(std::min(100, load.percent)), 100);
  }
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, 330, detail.c_str());
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void NativeProviderBookActivity::renderError() {
  const auto labels = mappedInput.mapLabels("« 返回", "重试", "", "");
  std::vector<std::string> diag;
  M4ErrorScreen::appendCode(diag, error_);
  M4ErrorScreen::addKV(diag, "provider: ", providerId_);
  M4ErrorScreen::addKV(diag, "book: ", bookId_);
  M4ErrorScreen::addKV(diag, "app: ", appId_);
  {
    char b[48];
    std::snprintf(b, sizeof(b), "state: %d", static_cast<int>(state_));
    M4ErrorScreen::add(diag, b);
  }
  auto snap = M4ErrorScreen::genericFail(title_.empty() ? "在线阅读" : title_, "无法打开", error_, diag,
                                         labels.btn1, labels.btn2);
  M4ErrorScreen::paint(renderer, snap, true);
}

void NativeProviderBookActivity::loop() {
  if (subActivity) {
    const bool closed = pumpSubActivityFrame();
    if (!closed) return;

    if (state_ == State::Toc) {
      if (tocBackPending_) {
        tocBackPending_ = false;
        state_ = State::Detail;
        renderDetail();
        return;
      }
      if (tocSelectionPending_ && tocSelectedIndex_ >= 0) {
        const int next = tocSelectedIndex_;
        tocSelectionPending_ = false;
        requestChapter(next, true);
        return;
      }
    } else if (state_ == State::Login && loginFinishedPending_) {
      loginFinishedPending_ = false;
      if (loginSucceeded_ && loadingIndex_ >= 0) {
        requestChapter(loadingIndex_, loadingFromToc_);
      } else if (loginSucceeded_ && pendingCatalogAction_ != PendingCatalogAction::None) {
        if (prepareCatalog()) continueAfterCatalogReady();
        else if (!startCatalogBootstrap(pendingCatalogAction_)) {
          state_ = State::Error;
          if (error_.empty()) error_ = "登录成功，但目录加载失败";
          renderError();
        }
      } else {
        state_ = State::Detail;
        renderDetail();
      }
      return;
    } else if (state_ == State::Reader && readerBackPending_) {
      readerBackPending_ = false;
      if (tocSelectionPending_ && tocSelectedIndex_ >= 0) {
        const int next = tocSelectedIndex_;
        tocSelectionPending_ = false;
        requestChapter(next, true);
      } else {
        state_ = State::Detail;
        renderDetail();
      }
      return;
    }
  }

  if (state_ == State::Detail) {
    pollDetailLoading();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      onExitBook_();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startReading();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      openToc();
      return;
    }
    if (mappedInput.hasTouch()) {
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        if (detailReadButtonHeight_ > 0 && ty >= detailReadButtonTop_ &&
            ty < detailReadButtonTop_ + detailReadButtonHeight_) {
          startReading();
          return;
        }
        const auto metrics = UITheme::getInstance().getMetrics();
        if (ty >= renderer.getScreenHeight() - metrics.buttonHintsHeight) {
          const int slot = std::min(3, std::max(0, tx * 4 / std::max(1, renderer.getScreenWidth())));
          if (slot == 0) onExitBook_();
          else if (slot == 1) startReading();
          else if (slot == 2) openToc();
        }
      }
    }
    return;
  }

  if (state_ == State::CatalogLoading) {
    if (catalogStartPending_) {
      renderCatalogLoading(false);
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
        catalogStartPending_ = false;
        pendingCatalogAction_ = PendingCatalogAction::None;
        state_ = State::Detail;
        renderDetail();
        return;
      }
      if (static_cast<int32_t>(millis() - catalogStartAtMs_) < 0) return;
      catalogStartPending_ = false;
      if (!M4NativeProviderCatalog::start(providerId_, bookId_, appId_, title_, currentIndex_)) {
        error_ = "目录任务启动失败";
        state_ = State::Error;
        renderError();
        return;
      }
    }
    const auto c = M4NativeProviderCatalog::snapshot();
    const bool mine = c.providerId == providerId_ && c.bookId == bookId_ && c.appId == appId_;
    if (mine && c.phase == M4NativeProviderCatalog::Phase::Ready) {
      if (prepareCatalog()) {
        continueAfterCatalogReady();
      } else {
        state_ = State::Error;
        if (error_.empty()) error_ = "目录注册后无法读取";
        renderError();
      }
      return;
    }
    if (mine && c.phase == M4NativeProviderCatalog::Phase::Error &&
        (c.error == "catalog_commit_failed" || c.error == "sd_open_failed") && prepareCatalog()) {
      Serial.printf("[NativeBook] catalog commit err but file usable → continue\n");
      continueAfterCatalogReady();
      return;
    }
    renderCatalogLoading(false);
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      M4NativeProviderCatalog::cancel();
      pendingCatalogAction_ = PendingCatalogAction::None;
      state_ = State::Detail;
      renderDetail();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mine) {
      if (c.phase == M4NativeProviderCatalog::Phase::AuthRequired) {
        openLogin();
      } else if (c.phase == M4NativeProviderCatalog::Phase::Error) {
        const PendingCatalogAction action = pendingCatalogAction_ == PendingCatalogAction::None
                                                ? PendingCatalogAction::OpenToc
                                                : pendingCatalogAction_;
        (void)startCatalogBootstrap(action);
      }
    }
    return;
  }

  if (state_ == State::Loading) {
    if (chapterStartPending_) {
      renderLoading(false);
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
        chapterStartPending_ = false;
        if (loadingFromToc_) openToc();
        else {
          state_ = State::Detail;
          renderDetail();
        }
        return;
      }
      if (static_cast<int32_t>(millis() - chapterStartAtMs_) < 0) return;
      chapterStartPending_ = false;
      const bool queued = M4NativeProviderManager::requestChapter(
          providerId_, bookId_, loadingIndex_, M4NativeProviderManager::LoadIntent::Foreground);
      if (queued) chapterLoadStartedAtMs_ = millis();
      if (!queued) {
        const auto queuedState = M4ContentProviderSession::chapterAt(providerId_, bookId_, loadingIndex_);
        if (queuedState.state != M4ContentProvider::ChapterReady::Ready) {
          error_ = chapterErrorText(queuedState.error.empty() ? "chapter_queue_failed" : queuedState.error);
          state_ = State::Error;
          renderError();
          return;
        }
      }
    }
    const auto st = M4ContentProviderSession::chapterAt(providerId_, bookId_, loadingIndex_);
    const auto p = M4NativeProviderManager::progress();
    const bool authRequired = isAuthError(st.error) ||
                              (p.providerId == providerId_ && p.bookId == bookId_ &&
                               p.chapterIndex0 == loadingIndex_ &&
                               p.phase == M4NativeProvider::Phase::AuthRequired);
    if (st.state == M4ContentProvider::ChapterReady::Ready) {
      if (!openReadyReader(loadingIndex_)) {
        state_ = State::Error;
        if (error_.empty()) error_ = "章节打开失败";
        renderError();
      }
      return;
    }
    if (st.state == M4ContentProvider::ChapterReady::Error && authRequired) {
      openLogin();
      return;
    }
    if (chapterLoadStartedAtMs_ != 0 && st.state != M4ContentProvider::ChapterReady::Error &&
        millis() - chapterLoadStartedAtMs_ >= kChapterLoadingTimeoutMs) {
      M4NativeProviderManager::cancelForeground();
      error_ = "chapter_timeout";
      state_ = State::Error;
      renderError();
      return;
    }
    renderLoading(false);
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      M4NativeProviderManager::cancelForeground();
      if (loadingFromToc_) openToc();
      else {
        state_ = State::Detail;
        renderDetail();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (authRequired) openLogin();
      else if (st.state == M4ContentProvider::ChapterReady::Error) {
        (void)M4NativeProviderManager::invalidateChapterCache(providerId_, bookId_, loadingIndex_);
        requestChapter(loadingIndex_, loadingFromToc_);
      }
    }
    return;
  }

  if (state_ == State::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      loadingIndex_ = -1;
      error_.clear();
      state_ = State::Detail;
      renderDetail();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (loadingIndex_ >= 0) {
        (void)M4NativeProviderManager::invalidateChapterCache(providerId_, bookId_, loadingIndex_);
        requestChapter(loadingIndex_, loadingFromToc_);
      } else {
        const PendingCatalogAction action = pendingCatalogAction_ == PendingCatalogAction::None
                                                ? PendingCatalogAction::OpenToc
                                                : pendingCatalogAction_;
        if (!startCatalogBootstrap(action)) renderError();
      }
    }
  }
}

std::string NativeProviderBookActivity::debugUiJson() {
  return std::string("{\"kind\":\"native_provider_book\",\"provider\":\"") + providerId_ +
         "\",\"book\":\"" + bookId_ + "\",\"chapter\":" + std::to_string(currentIndex_) +
         ",\"state\":" + std::to_string(static_cast<int>(state_)) +
         ",\"detail_loaded\":" + (detail_.intro.empty() ? "false" : "true") +
         ",\"detail_loading\":" + (detailLoading_ ? "true" : "false") +
         ",\"detail_error\":\"" + detailError_ + "\"}";
}

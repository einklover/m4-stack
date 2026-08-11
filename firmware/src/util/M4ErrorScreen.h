#pragma once

// Shared error surface for photo-debug: big human title + Chinese reason, then
// SMALL_FONT diagnostic lines (raw codes, metrics, free space) that fit on a
// phone camera snapshot. Used by catalog/chapter/native app/runtime/screen bridge.

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace M4ErrorScreen {

struct Line {
  std::string text;
};

struct Snapshot {
  std::string header;       // header bar title
  std::string title;        // bold, e.g. "目录加载失败"
  std::string reason;       // human Chinese, optional
  std::string detail;       // mid line (progress / subtitle), optional
  std::vector<std::string> diag;  // SMALL_FONT left-aligned diagnostics
  const char* btn1 = "« 返回";
  const char* btn2 = "";
  const char* btn3 = "";
  const char* btn4 = "";
  bool fastRefresh = true;
};

inline void add(std::vector<std::string>& out, const std::string& line) {
  if (line.empty()) return;
  // Cap per-line length for readable photos; wrap is handled at draw time.
  if (line.size() > 160) {
    out.push_back(line.substr(0, 157) + "...");
  } else {
    out.push_back(line);
  }
}

inline void addKV(std::vector<std::string>& out, const char* key, const std::string& value) {
  if (!key || value.empty()) return;
  add(out, std::string(key) + value);
}

inline void addKV(std::vector<std::string>& out, const char* key, size_t n) {
  if (!key) return;
  char b[48];
  std::snprintf(b, sizeof(b), "%s%u", key, static_cast<unsigned>(n));
  add(out, b);
}

inline std::string formatBytes(size_t n) {
  char b[32];
  if (n >= 1024u * 1024u) {
    std::snprintf(b, sizeof(b), "%.2fMB", static_cast<double>(n) / (1024.0 * 1024.0));
  } else if (n >= 1024u) {
    std::snprintf(b, sizeof(b), "%.1fKB", static_cast<double>(n) / 1024.0);
  } else {
    std::snprintf(b, sizeof(b), "%uB", static_cast<unsigned>(n));
  }
  return b;
}

// Free/total on the active SD volume (best-effort; 0 if unknown).
inline void appendSdSpace(std::vector<std::string>& out) {
  auto& sd = SDCardManager::getInstance();
  if (!sd.ready()) {
    add(out, "sd: not ready");
    return;
  }
  const uint64_t total = sd.sdTotalBytes();
  const uint64_t used = sd.sdUsedBytes();
  if (total == 0) {
    add(out, "sd: mounted (size n/a)");
    return;
  }
  const uint64_t freeB = total > used ? total - used : 0;
  char b[96];
  std::snprintf(b, sizeof(b), "sd free %s / total %s", formatBytes(static_cast<size_t>(freeB)).c_str(),
                formatBytes(static_cast<size_t>(total)).c_str());
  add(out, b);
  if (freeB < 2u * 1024u * 1024u) {
    add(out, "hint: SD free < 2MB, write may fail");
  }
}

inline void appendCode(std::vector<std::string>& out, const std::string& code) {
  if (code.empty()) {
    add(out, "code: (empty)");
  } else {
    addKV(out, "code: ", code);
  }
}

// Draw a full-screen error. Caller supplies MappedInputManager labels already
// resolved into Snapshot.btn*. Does not call displayBuffer — returns so caller
// can pass refresh mode, OR pass paintAndFlush=true.
inline int contentTop(const ThemeMetrics& metrics) {
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

inline void paint(GfxRenderer& renderer, const Snapshot& s, bool paintAndFlush = true) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int pad = std::max(12, metrics.contentSidePadding);
  const int textW = std::max(40, w - 2 * pad);
  const int footerReserve = metrics.buttonHintsHeight + 8;

  if (!s.header.empty()) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, s.header.c_str());
  }

  int y = contentTop(metrics) + 8;
  if (!s.title.empty()) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, y, s.title.c_str(), true, EpdFontFamily::BOLD);
    y += 40;
  }
  if (!s.reason.empty()) {
    auto lines = M4UiText::wrapLines(renderer, UI_10_FONT_ID, s.reason.c_str(), textW, 3);
    for (const auto& ln : lines) {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, ln.c_str());
      y += 28;
    }
    y += 4;
  }
  if (!s.detail.empty()) {
    auto lines = M4UiText::wrapLines(renderer, UI_10_FONT_ID, s.detail.c_str(), textW, 2);
    for (const auto& ln : lines) {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, ln.c_str());
      y += 26;
    }
    y += 6;
  }

  // Divider label so photos make clear the next block is machine-readable.
  if (!s.diag.empty()) {
    M4UiText::drawMuted(renderer, SMALL_FONT_ID, pad, y, "诊断信息(拍照可排查)");
    y += 22;
  }

  const int maxY = h - footerReserve - 4;
  for (const auto& raw : s.diag) {
    if (y + 18 > maxY) {
      M4UiText::drawMuted(renderer, SMALL_FONT_ID, pad, y, "...(truncated)");
      break;
    }
    auto lines = M4UiText::wrapLines(renderer, SMALL_FONT_ID, raw.c_str(), textW, 3);
    if (lines.empty()) continue;
    for (const auto& ln : lines) {
      if (y + 18 > maxY) break;
      M4UiText::draw(renderer, SMALL_FONT_ID, pad, y, ln.c_str(), true, EpdFontFamily::REGULAR);
      y += 18;
    }
  }

  // Button strip: only if at least one label is non-empty (caller may draw own).
  if ((s.btn1 && s.btn1[0]) || (s.btn2 && s.btn2[0]) || (s.btn3 && s.btn3[0]) || (s.btn4 && s.btn4[0])) {
    GUI.drawButtonHints(renderer, s.btn1 ? s.btn1 : "", s.btn2 ? s.btn2 : "", s.btn3 ? s.btn3 : "",
                        s.btn4 ? s.btn4 : "", true);
  }

  if (paintAndFlush) {
    renderer.displayBuffer(s.fastRefresh ? HalDisplay::FAST_REFRESH : HalDisplay::FULL_REFRESH);
  }
}

// Convenience builders for common scopes.

inline Snapshot catalogFail(const std::string& headerTitle, const std::string& reasonZh,
                            const std::string& code, const std::string& providerId,
                            const std::string& bookId, const std::string& appId, size_t receivedBytes,
                            size_t rows, uint32_t elapsedSec, const char* btnBack = "« 返回",
                            const char* btnRetry = "重试") {
  Snapshot s;
  s.header = headerTitle.empty() ? "在线阅读" : headerTitle;
  s.title = "目录加载失败";
  s.reason = reasonZh.empty() ? "未知原因" : reasonZh;
  char detail[96];
  std::snprintf(detail, sizeof(detail), "接收 %s · %u 项 · %us", formatBytes(receivedBytes).c_str(),
                static_cast<unsigned>(rows), static_cast<unsigned>(elapsedSec));
  s.detail = detail;
  appendCode(s.diag, code);
  addKV(s.diag, "provider: ", providerId);
  addKV(s.diag, "book: ", bookId);
  addKV(s.diag, "app: ", appId);
  addKV(s.diag, "recv: ", formatBytes(receivedBytes));
  addKV(s.diag, "rows: ", rows);
  {
    char b[40];
    std::snprintf(b, sizeof(b), "elapsed: %us", static_cast<unsigned>(elapsedSec));
    add(s.diag, b);
  }
  appendSdSpace(s.diag);
  if (code == "catalog_commit_failed" || code == "sd_open_failed" || code == "sink_failed" ||
      code == "sink_write_failed") {
    add(s.diag, "hint: check SD free space / reinsert card");
  }
  if (code == "response_too_large") {
    add(s.diag, "hint: TOC JSON exceeded host maxBytes (need firmware with 4MB cap)");
  }
  if (code == "legado_endpoint_missing" || code == "book_locator_missing") {
    add(s.diag, "hint: open Legado transfer page on phone");
  }
  s.btn1 = btnBack;
  s.btn2 = btnRetry;
  return s;
}

inline Snapshot chapterFail(const std::string& headerTitle, const std::string& chapterTitle,
                            const std::string& reasonZh, const std::string& code,
                            const std::string& providerId, const std::string& bookId, int chapterIndex0,
                            size_t receivedBytes, size_t writtenBytes, int percent,
                            uint32_t elapsedSec, const char* btnBack = "« 返回",
                            const char* btnRetry = "重试") {
  Snapshot s;
  s.header = headerTitle.empty() ? "在线阅读" : headerTitle;
  s.title = "章节加载失败";
  s.reason = reasonZh.empty() ? "未知原因" : reasonZh;
  if (!chapterTitle.empty()) s.detail = chapterTitle;
  appendCode(s.diag, code);
  addKV(s.diag, "provider: ", providerId);
  addKV(s.diag, "book: ", bookId);
  {
    char b[48];
    std::snprintf(b, sizeof(b), "chapter_index0: %d", chapterIndex0);
    add(s.diag, b);
  }
  addKV(s.diag, "recv: ", formatBytes(receivedBytes));
  addKV(s.diag, "written: ", formatBytes(writtenBytes));
  if (percent > 0) {
    char b[32];
    std::snprintf(b, sizeof(b), "percent: %d", percent);
    add(s.diag, b);
  }
  {
    char b[40];
    std::snprintf(b, sizeof(b), "elapsed: %us", static_cast<unsigned>(elapsedSec));
    add(s.diag, b);
  }
  appendSdSpace(s.diag);
  s.btn1 = btnBack;
  s.btn2 = btnRetry;
  return s;
}

inline Snapshot genericFail(const std::string& headerTitle, const std::string& title,
                            const std::string& reasonZh, const std::vector<std::string>& extraDiag,
                            const char* btnBack = "« 返回", const char* btnRetry = "") {
  Snapshot s;
  s.header = headerTitle;
  s.title = title.empty() ? "出错了" : title;
  s.reason = reasonZh;
  s.diag = extraDiag;
  appendSdSpace(s.diag);
  s.btn1 = btnBack;
  s.btn2 = btnRetry;
  return s;
}

}  // namespace M4ErrorScreen

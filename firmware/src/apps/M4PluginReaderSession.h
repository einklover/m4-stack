#pragma once

// Thread-safe handoff queue between Lua owner task and AppRuntime UI task.

#include "util/M4PluginReaderBridge.h"
#include "apps/providers/M4NativeProviderManager.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace M4PluginReaderSession {

struct ProgressSnapshot {
  int page = 0;
  int total = -1;
  size_t byteOffset = 0;
  bool complete = false;
  char bookId[M4PluginReaderBridge::kMaxIdLen + 1] = {};
  char chapterUid[M4PluginReaderBridge::kMaxIdLen + 1] = {};
  char progressKey[M4PluginReaderBridge::kMaxProgressKeyLen + 1] = {};
  bool pendingDeliver = false;
  uint32_t generation = 0;
  char error[32] = {};
  bool openFailed = false;
  int switchChapterIndex = -1;
};

inline std::mutex& mu() {
  static std::mutex m;
  return m;
}

inline M4PluginReaderBridge::OpenRequest& pendingOpen() {
  static M4PluginReaderBridge::OpenRequest r;
  return r;
}

inline std::atomic<bool>& openReady() {
  static std::atomic<bool> v{false};
  return v;
}

inline std::atomic<int>& fallbackSwitchChapterIndex() {
  static std::atomic<int> v{-1};
  return v;
}

inline int pendingFallbackSwitchChapterIndex() {
  return fallbackSwitchChapterIndex().load(std::memory_order_acquire);
}

inline int takeFallbackSwitchChapterIndex() {
  return fallbackSwitchChapterIndex().exchange(-1, std::memory_order_acq_rel);
}

inline std::atomic<bool>& launchInProgress() {
  static std::atomic<bool> v{false};
  return v;
}

inline std::atomic<uint32_t>& generation() {
  static std::atomic<uint32_t> g{1};
  return g;
}

inline ProgressSnapshot& progressSlot() {
  static ProgressSnapshot p;
  return p;
}

inline std::string& boundAppId() {
  static std::string id;
  return id;
}

inline uint32_t bumpGeneration() { return generation().fetch_add(1, std::memory_order_relaxed) + 1; }

struct TocRequest {
  std::string tocRelPath;
  std::string tocAbsPath;
  std::string providerId;
  std::string appDataRoot;
  std::string bookTitle;
  std::string bookId;
  std::string appId;
  int currentIndex = 0;
  uint32_t generation = 0;
};

struct TocResult {
  bool pendingDeliver = false;
  bool cancelled = true;
  int chapterIndex = -1;
  char bookId[M4PluginReaderBridge::kMaxIdLen + 1] = {};
  uint32_t generation = 0;
};

inline TocRequest& pendingToc() {
  static TocRequest r;
  return r;
}

inline std::atomic<bool>& tocReady() {
  static std::atomic<bool> v{false};
  return v;
}

inline TocResult& tocResultSlot() {
  static TocResult r;
  return r;
}

inline void clearPendingOpen() {
  std::lock_guard<std::mutex> lock(mu());
  openReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(false, std::memory_order_relaxed);
  pendingOpen() = {};
}

inline void clearPendingToc() {
  std::lock_guard<std::mutex> lock(mu());
  tocReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(false, std::memory_order_relaxed);
  pendingToc() = {};
}

inline void clearProgress() {
  std::lock_guard<std::mutex> lock(mu());
  progressSlot() = {};
}

inline void clearForApp(const std::string& appId) {
  std::lock_guard<std::mutex> lock(mu());
  boundAppId() = appId;
  openReady().store(false, std::memory_order_relaxed);
  tocReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(false, std::memory_order_relaxed);
  fallbackSwitchChapterIndex().store(-1, std::memory_order_release);
  pendingOpen() = {};
  pendingToc() = {};
  progressSlot() = {};
  tocResultSlot() = {};
  generation().fetch_add(1, std::memory_order_relaxed);
}

inline bool queueOpen(const M4PluginReaderBridge::OpenRequest& req) {
  // Native provider chapter-end handoff: do not manufacture the legacy
  // close->Lua fallback. The reader already queued ContentProvider work;
  // ensureChapter starts the single native worker and keeps TxtReader resident.
  // A subsequent tap (or native picker path) can switch immediately once the
  // ChapterStatus becomes Ready. No Lua/TLS overlap is introduced.
  if (!req.providerId.empty() && req.chapterIndex >= 0 && req.absPath.empty() &&
      M4NativeProviderManager::supports(req.providerId)) {
    if (M4NativeProviderManager::ensureChapter(req.providerId, req.bookId, req.chapterIndex, true)) {
      std::lock_guard<std::mutex> lock(mu());
      openReady().store(false, std::memory_order_relaxed);
      pendingOpen() = {};
      fallbackSwitchChapterIndex().store(-1, std::memory_order_release);
      return true;
    }
  }

  std::lock_guard<std::mutex> lock(mu());
  if (!boundAppId().empty() && req.appId != boundAppId()) return false;

  // Compatibility fallback for providers without a native adapter.
  if (!req.providerId.empty() && req.chapterIndex >= 0 && req.absPath.empty()) {
    openReady().store(false, std::memory_order_relaxed);
    pendingOpen() = {};
    tocReady().store(false, std::memory_order_relaxed);
    pendingToc() = {};
    fallbackSwitchChapterIndex().store(req.chapterIndex, std::memory_order_release);
    return true;
  }

  fallbackSwitchChapterIndex().store(-1, std::memory_order_release);
  tocReady().store(false, std::memory_order_relaxed);
  pendingToc() = {};
  pendingOpen() = req;
  openReady().store(true, std::memory_order_release);
  return true;
}

inline bool takeOpen(M4PluginReaderBridge::OpenRequest& out) {
  if (!openReady().load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(mu());
  if (!openReady().load(std::memory_order_relaxed)) return false;
  out = pendingOpen();
  openReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(true, std::memory_order_release);
  return true;
}

inline bool queueToc(const TocRequest& req) {
  std::lock_guard<std::mutex> lock(mu());
  if (!boundAppId().empty() && req.appId != boundAppId()) return false;
  fallbackSwitchChapterIndex().store(-1, std::memory_order_release);
  openReady().store(false, std::memory_order_relaxed);
  pendingOpen() = {};
  pendingToc() = req;
  tocReady().store(true, std::memory_order_release);
  return true;
}

inline bool takeToc(TocRequest& out) {
  if (!tocReady().load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(mu());
  if (!tocReady().load(std::memory_order_relaxed)) return false;
  out = pendingToc();
  tocReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(true, std::memory_order_release);
  return true;
}

inline void clearLaunchInProgress() { launchInProgress().store(false, std::memory_order_release); }

inline bool handoffBlocksLuaDisplay() {
  return openReady().load(std::memory_order_acquire) || tocReady().load(std::memory_order_acquire) ||
         launchInProgress().load(std::memory_order_acquire);
}

inline void publishTocResult(const TocResult& r) {
  std::lock_guard<std::mutex> lock(mu());
  tocResultSlot() = r;
  tocResultSlot().pendingDeliver = true;
}

inline bool takeTocResult(TocResult& out) {
  std::lock_guard<std::mutex> lock(mu());
  if (!tocResultSlot().pendingDeliver) return false;
  out = tocResultSlot();
  tocResultSlot().pendingDeliver = false;
  return true;
}

inline void publishProgress(const ProgressSnapshot& p) {
  std::lock_guard<std::mutex> lock(mu());
  progressSlot() = p;
  progressSlot().pendingDeliver = true;
}

inline bool takeProgress(ProgressSnapshot& out, uint32_t expectGeneration = 0) {
  std::lock_guard<std::mutex> lock(mu());
  if (!progressSlot().pendingDeliver) return false;
  const uint32_t gen = progressSlot().generation;
  const uint32_t cur = generation().load(std::memory_order_relaxed);
  if (expectGeneration != 0 && gen != expectGeneration) {
    progressSlot().pendingDeliver = false;
    return false;
  }
  if (gen == 0 || cur == 0 || gen != cur) {
    progressSlot().pendingDeliver = false;
    return false;
  }
  out = progressSlot();
  progressSlot().pendingDeliver = false;
  return true;
}

inline uint32_t currentGeneration() { return generation().load(std::memory_order_relaxed); }

}  // namespace M4PluginReaderSession

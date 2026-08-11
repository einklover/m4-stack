#pragma once

#include "apps/M4FileRowSource.h"
#include "apps/M4xLuaSandbox.h"
#include "apps/M4FileRowSource.h"
#include "apps/M4xRegistry.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
class HTTPClient;
class NetworkClientSecure;

// Sandboxed Lua host for one installed app session.
// Not a strong security boundary: enforces memory + instruction/time budgets
// and path allow-lists so a buggy plugin cannot easily hard-WDT the whole device.
//
// Threading: all start/draw/key/touch/stop must run on one owner task.
// requestCancel() is the only method safe to call from another task (atomic).
class M4xLuaHost {
 public:
  M4xLuaHost();
  ~M4xLuaHost();

  M4xLuaHost(const M4xLuaHost&) = delete;
  M4xLuaHost& operator=(const M4xLuaHost&) = delete;

  // Load entry script and call init(). renderer used for gui.* bindings.
  bool start(GfxRenderer& renderer, const M4xInstalledApp& app, std::string& errorOut);

  // Call draw() if present; returns false on lua error / budget violation.
  bool callDraw(std::string& errorOut);

  // Optional input hooks.
  bool callOnKey(const char* keyName, std::string& errorOut);
  bool callOnTouch(int x, int y, const char* phase, std::string& errorOut);

  bool wantsExit() const { return exitRequested_; }
  void requestExit() { exitRequested_ = true; }

  // True when Lua global `dirty` is set — host should schedule a fast Tick
  // (UI-first chapter load / ContentProvider cooperative pipeline).
  bool wantsPump() const;
  // Optional Lua `frame_changed` hint. Missing/non-boolean means display.
  // Lets cooperative loading ticks keep dirty=true (pump backend) without
  // physically refreshing identical AA text frames.
  bool frameChanged() const;

  // Cooperative cancel: safe from UI task. Host ops and Lua hook check this.
  void requestCancel() { cancelRequested_.store(true, std::memory_order_relaxed); }
  void clearCancel() { cancelRequested_.store(false, std::memory_order_relaxed); }
  bool isCancelRequested() const { return cancelRequested_.load(std::memory_order_relaxed); }

  // Owner-task only.
  void stop();

  size_t luaMemUsed() const { return budget_.memUsed; }
  size_t luaMemPeak() const { return budget_.memPeak; }
  size_t luaMemLimit() const { return budget_.memLimit; }
  // Remaining Lua heap budget for new allocations (0 if over limit).
  size_t luaMemHeadroom() const {
    if (budget_.memUsed >= budget_.memLimit) return 0;
    return budget_.memLimit - budget_.memUsed;
  }

  const std::string& dataDir() const { return dataDir_; }

  // Notify plugin that native reader closed (owner task only).
  // Calls global onReaderClosed(table) if present; always sets reader_last_progress.
  // openError non-null/non-empty means open/load failed (not a successful close).
  // switchChapterIndex: 0-based, <0 means none (user did not pick another chapter).
  bool callOnReaderClosed(int page, int total, size_t byteOffset, bool complete, const char* bookId,
                          const char* chapterUid, const char* progressKey, std::string& errorOut,
                          const char* openError = nullptr, int switchChapterIndex = -1);

  // Notify plugin that native system TOC closed (owner task only).
  // Calls global onTocClosed(table); always sets reader_last_toc.
  bool callOnTocClosed(bool cancelled, int chapterIndex0, const char* bookId, std::string& errorOut);

  // Cooperative ContentProvider prefetch while native reader owns the panel.
  // Calls global provider_pump_work() if present; never paints.
  bool callProviderPump(std::string& errorOut);
  bool loaderNeedsPump() const;

  // Read plugin globals + host list scene into a JSON object (no outer array).
  // Safe when L_ is null (returns {"lua":false}). Owner-task only.
  std::string debugUiJson() const;

  // Accessed by Lua C bindings in M4xLuaHost.cpp
  GfxRenderer* renderer_ = nullptr;
  M4xInstalledApp app_{};
  std::string dataDir_;   // /apps_data/<id>
  std::string installDir_;  // /apps/<id>

  // One keep-alive TLS connection reused by dl.jsonGet (and later dl.*).
  // A fresh handshake per request fragments internal RAM with mbedTLS session
  // buffers, and the 40KB jsonGet gate then blocks later fetches (jjwxc
  // "list too large; back to shelf and retry"). HTTPClient itself swaps the
  // connection when the host changes; a dead connection is rebuilt on the
  // next request (GET<0 path). Owner-task only.
  std::unique_ptr<NetworkClientSecure> netTls_;
  std::unique_ptr<HTTPClient> netHttp_;
  // Drop the keep-alive TLS connection now (frees ~32KB internal RAM that the
  // mbedTLS session retains). Called before entering the native reader, where
  // no networking happens — keeps internal headroom for the next chapter.
  void releaseNetworkSession();

  // True only when this host established Wi-Fi via net.connectSaved.
  // On stop, disconnect only if owned (never tear down another component's link).
  bool wifiOwned_ = false;

  // Extend current Lua callback wall clock so blocking C APIs (connectSaved)
  // do not trip lua_time_limit immediately on return to Lua.
  void extendCallbackWallMs(uint32_t ms) {
    if (budget_.wallLimitMs == 0) return;
    const uint32_t now = budget_.nowMs ? budget_.nowMs() : 0;
    const uint32_t elapsed = now >= budget_.wallStartMs ? (now - budget_.wallStartMs) : 0;
    const uint32_t need = elapsed + ms + 2000;
    if (need > budget_.wallLimitMs) budget_.wallLimitMs = need;
  }

  // ---- Host-owned list scene (ui.list*) --------------------------------
  // While active, the host renders the list (rows + pagination + footer
  // buttons) and routes input to Lua callbacks; Lua must not paint.
  struct UiListScene {
    bool active = false;
    std::string title;
    std::string footer;
    std::vector<std::string> rows;
    std::vector<std::string> rowSubs;
    // A subtitle row uses the same two-line row rhythm as the native system
    // lists.  The host keeps this bit so drawing, pagination and touch hit
    // testing all select the same row height.
    bool hasSubtitles = false;
    int pageSize = 12;
    int page = 1;
    // Optional remote-page count for providers that fetch one API page at a
    // time. Keeps footer navigation active while one page is resident.
    int pageCountOverride = 0;
    // When true, rows contains only the currently resident remote page. The
    // host renders that vector from row zero while callbacks still receive a
    // global zero-based index. This avoids padding/accumulating all earlier
    // pages in Lua just to make the native list pageable.
    bool remotePage = false;
    std::string onRow;   // fn(index0, title, sub)  | file: fn(index0, field0, field1, ...)
    std::string onPage;  // fn(page, totalPages)
    std::string onBack;  // fn()
    bool repaint = true;
    uint32_t generation = 0;
    // File-backed row source ("virtual memory"): rows live on SD, not the Lua
    // heap. Each line is tab-separated field values; only one page is
    // materialized on demand through the bounded seekable source.
    bool fromFile = false;
    std::shared_ptr<M4FileRows::FileRowSource> fileSource;
  };

  bool uiSceneActive() const { return uiScene_.active; }
  bool uiSceneRepaintRequested() const { return uiScene_.active && uiScene_.repaint; }
  void clearUiSceneRepaint() { uiScene_.repaint = false; }
  void requestUiSceneRepaint() { uiScene_.repaint = true; }
  // Owner-task only. Renders the scene; returns false when the scene closed
  // mid-render (caller should fall through to the normal Lua draw path).
  bool renderUiScene();
  // Owner-task only. Returns true when the input was consumed by the scene.
  // On Lua callback error fills errorOut (caller applies the failed state).
  bool handleUiSceneKey(const char* key, std::string& errorOut);
  bool handleUiSceneTouch(int x, int y, const char* phase, std::string& errorOut);
  void closeUiScene();
  UiListScene& uiScene() { return uiScene_; }
  const UiListScene& uiScene() const { return uiScene_; }

 private:
  bool uiCallRow(int index0, std::string& errorOut);
  bool uiCallPage(int newPage, std::string& errorOut);
  bool uiCallBack(std::string& errorOut);
  bool uiCallGlobal(const char* fn, std::string& errorOut, int nargs);

 private:
  void* L_ = nullptr;  // lua_State*
  bool exitRequested_ = false;
  std::atomic<bool> cancelRequested_{false};
  M4xLuaSandbox::Budget budget_{};
  UiListScene uiScene_{};
};

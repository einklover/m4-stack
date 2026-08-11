#include "AppRuntimeActivity.h"

#include "activities/reader/TxtReaderActivity.h"
#include "activities/reader/TxtReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <Txt.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/M4ErrorScreen.h"
#include "util/M4UiText.h"
#include "util/M4InputProfile.h"
#include "util/M4PluginReaderBridge.h"
#include "util/M4PluginTocList.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/M4PluginReaderSession.h"
#include "RecentBooksStore.h"

#include <cstring>
#include <memory>
#include <vector>

namespace {
constexpr UBaseType_t kEventQueueLen = 24;
constexpr uint32_t kQueueSendTicks = pdMS_TO_TICKS(50);
constexpr uint32_t kIdleDrawMs = 2000;
// ContentProvider / chapter load sets Lua dirty=true: pump cooperative steps
// quickly so uncached chapter hops do not sit idle 2s between UI updates.
constexpr uint32_t kPumpIdleDrawMs = 50;
constexpr uint32_t kRecvTimeoutMs = 30;
constexpr uint32_t kJoinWarnMs = 5000;
}  // namespace

AppRuntimeActivity::AppRuntimeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, M4xInstalledApp app,
                                       const std::function<void()>& onExitApp)
    : ActivityWithSubactivity("AppRuntime", renderer, mappedInput), app_(std::move(app)), onExitApp_(onExitApp) {}

void AppRuntimeActivity::taskTrampoline(void* param) {
  static_cast<AppRuntimeActivity*>(param)->runtimeTaskMain();
}

bool AppRuntimeActivity::postEvent(const M4xRuntime::Event& e) {
  if (!eventQueue_ || life_.isStopRequested() || life_.isDone()) return false;
  if (xQueueSend(eventQueue_, &e, kQueueSendTicks) != pdTRUE) {
    M4xRuntime::Event discard;
    xQueueReceive(eventQueue_, &discard, 0);
    return xQueueSend(eventQueue_, &e, 0) == pdTRUE;
  }
  return true;
}

void AppRuntimeActivity::setFailed(const std::string& err) {
  failed_.store(true, std::memory_order_relaxed);
  if (errorMutex_ && xSemaphoreTake(errorMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    std::strncpy(errorBuf_, err.c_str(), sizeof(errorBuf_) - 1);
    errorBuf_[sizeof(errorBuf_) - 1] = '\0';
    xSemaphoreGive(errorMutex_);
  }
}

void AppRuntimeActivity::copyError(std::string& out) {
  out.clear();
  if (errorMutex_ && xSemaphoreTake(errorMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    out = errorBuf_;
    xSemaphoreGive(errorMutex_);
  }
}

void AppRuntimeActivity::handleEventOnOwner(const M4xRuntime::Event& e) {
  using M4xRuntime::EventType;
  std::string err;

  if (life_.shouldDrop(e.type)) return;

  switch (e.type) {
    case EventType::Start: {
      if (!host_.start(renderer, app_, err)) {
        setFailed(err.empty() ? "start_failed" : err);
        renderError();
      } else {
        ready_.store(true, std::memory_order_relaxed);
        if (host_.isCancelRequested() || life_.isStopRequested()) break;
        if (!host_.callDraw(err)) {
          setFailed(err.empty() ? "draw_failed" : err);
          renderError();
        } else {
          renderer.displayBuffer();
        }
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
      }
      break;
    }
    case EventType::Stop: {
      if (!life_.hostStopped) {
        host_.stop();
        life_.hostStopped = true;
      }
      ready_.store(false, std::memory_order_relaxed);
      M4PluginReaderSession::clearForApp(app_.id);
      M4ContentProviderSession::clearForApp(app_.id);
      break;
    }
    case EventType::Draw:
    case EventType::Tick: {
      if (failed_.load(std::memory_order_relaxed) || !ready_.load(std::memory_order_relaxed)) break;
      if (host_.isCancelRequested()) break;
      // Native TxtReader owns the physical display while it is the sub-activity.
      // Never callDraw/displayBuffer over it — that re-paints Lua loading and
      // makes first-frame handoff look like a hang/crash.
      if (subActivity) {
        // Still allow cooperative Lua work without painting: chapter prefetch
        // and history-reopen TOC/session restore (provider_pump_work is idle-cheap).
        if (pluginChildKind_ == PluginChildKind::Reader || host_.loaderNeedsPump()) {
          std::string pumpErr;
          if (!host_.callProviderPump(pumpErr) && !host_.isCancelRequested()) {
            Serial.printf("[WRCP] provider_pump err=%s\n", pumpErr.c_str());
          }
        }
        break;
      }
      // Host-owned ui.list scene: host renders rows/paging/buttons; Lua does
      // not paint while the scene is active.
      if (host_.uiSceneActive()) {
        std::string pumpErr;
        if (!host_.callProviderPump(pumpErr) && !host_.isCancelRequested()) {
          Serial.printf("[WRCP] provider_pump err=%s\n", pumpErr.c_str());
        }
        if (host_.uiSceneRepaintRequested()) {
          host_.clearUiSceneRepaint();
          if (host_.renderUiScene()) {
            renderer.displayBuffer();
          }
          // renderUiScene()==false means the scene closed inside a callback —
          // fall through so the next event paints the normal Lua frame.
        }
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      {
        M4PluginReaderSession::ProgressSnapshot snap;
        if (M4PluginReaderSession::takeProgress(snap)) {
          const char* openErr = (snap.openFailed && snap.error[0]) ? snap.error : nullptr;
          if (!host_.callOnReaderClosed(snap.page, snap.total, snap.byteOffset, snap.complete, snap.bookId,
                                        snap.chapterUid, snap.progressKey, err, openErr,
                                        snap.switchChapterIndex)) {
            if (!host_.isCancelRequested()) {
              setFailed(err.empty() ? "reader_closed_failed" : err);
              renderError();
              break;
            }
          }
        }
        M4PluginReaderSession::TocResult toc;
        if (M4PluginReaderSession::takeTocResult(toc)) {
          if (!host_.callOnTocClosed(toc.cancelled, toc.chapterIndex, toc.bookId, err)) {
            if (!host_.isCancelRequested()) {
              setFailed(err.empty() ? "toc_closed_failed" : err);
              renderError();
              break;
            }
          }
        }
      }
      if (!host_.callDraw(err)) {
        if (host_.isCancelRequested()) break;
        setFailed(err.empty() ? "draw_failed" : err);
        renderError();
        break;
      }
      // Sole launch site (owner task only). Never also launch from UI loop —
      // concurrent takeOpen/load vs displayBuffer races the e-ink BUSY line
      // (~30s timeout) and leaves the loading frame on panel.
      tryLaunchPluginReader();
      tryLaunchPluginToc();
      // A ui.list scene opened from within callDraw: host owns the display now.
      if (host_.uiSceneActive()) {
        if (host_.uiSceneRepaintRequested()) {
          host_.clearUiSceneRepaint();
          if (host_.renderUiScene()) renderer.displayBuffer();
        }
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      if (subActivity || M4PluginReaderSession::handoffBlocksLuaDisplay()) {
        Serial.printf("[WR05] t=%lu skip_lua_display sub=%d handoff=%d\n",
                      static_cast<unsigned long>(millis()), subActivity ? 1 : 0,
                      M4PluginReaderSession::handoffBlocksLuaDisplay() ? 1 : 0);
        break;
      }
      // Cooperative loading may keep dirty=true while visible pixels are
      // unchanged; skip the physical refresh (AA makes identical frames flash).
      if (!host_.frameChanged()) {
        Serial.printf("[WR05] t=%lu skip_unchanged_lua_frame\n",
                      static_cast<unsigned long>(millis()));
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      renderer.displayBuffer();
      if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
      break;
    }
    case EventType::Key: {
      if (failed_.load(std::memory_order_relaxed) || !ready_.load(std::memory_order_relaxed)) break;
      if (host_.isCancelRequested()) break;
      // Input is owned by the native reader sub-activity (UI loop pumps it).
      if (subActivity) break;
      // Host-owned ui.list scene consumes keys first.
      if (host_.uiSceneActive()) {
        std::string uiErr;
        if (!host_.handleUiSceneKey(e.key, uiErr)) {
          if (!uiErr.empty() && !host_.isCancelRequested()) {
            setFailed(uiErr.empty() ? "ui_scene_callback_failed" : uiErr);
            renderError();
            break;
          }
        }
        if (host_.uiSceneActive() && (host_.uiSceneRepaintRequested() || !uiErr.empty())) {
          host_.clearUiSceneRepaint();
          if (host_.renderUiScene()) renderer.displayBuffer();
        }
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      if (!host_.callOnKey(e.key, err)) {
        if (host_.isCancelRequested()) break;
        setFailed(err.empty() ? "key_failed" : err);
        renderError();
        break;
      }
      if (host_.isCancelRequested()) break;
      if (!host_.callDraw(err)) {
        if (host_.isCancelRequested()) break;
        setFailed(err.empty() ? "draw_failed" : err);
        renderError();
        break;
      }
      tryLaunchPluginReader();
      tryLaunchPluginToc();
      if (host_.uiSceneActive()) {
        if (host_.uiSceneRepaintRequested()) {
          host_.clearUiSceneRepaint();
          if (host_.renderUiScene()) renderer.displayBuffer();
        }
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      if (subActivity || M4PluginReaderSession::handoffBlocksLuaDisplay()) break;
      renderer.displayBuffer();
      if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
      break;
    }
    case EventType::Touch: {
      if (failed_.load(std::memory_order_relaxed) || !ready_.load(std::memory_order_relaxed)) break;
      if (host_.isCancelRequested()) break;
      if (subActivity) break;
      // Host-owned ui.list scene consumes touches first.
      if (host_.uiSceneActive()) {
        std::string uiErr;
        if (!host_.handleUiSceneTouch(e.x, e.y, e.phase, uiErr)) {
          if (!uiErr.empty() && !host_.isCancelRequested()) {
            setFailed(uiErr.empty() ? "ui_scene_callback_failed" : uiErr);
            renderError();
            break;
          }
        }
        if (host_.uiSceneActive() && (host_.uiSceneRepaintRequested() || !uiErr.empty())) {
          host_.clearUiSceneRepaint();
          if (host_.renderUiScene()) renderer.displayBuffer();
        }
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      if (!host_.callOnTouch(e.x, e.y, e.phase, err)) {
        if (host_.isCancelRequested()) break;
        setFailed(err.empty() ? "touch_failed" : err);
        renderError();
        break;
      }
      if (host_.isCancelRequested()) break;
      if (!host_.callDraw(err)) {
        if (host_.isCancelRequested()) break;
        setFailed(err.empty() ? "draw_failed" : err);
        renderError();
        break;
      }
      tryLaunchPluginReader();
      tryLaunchPluginToc();
      if (host_.uiSceneActive()) {
        if (host_.uiSceneRepaintRequested()) {
          host_.clearUiSceneRepaint();
          if (host_.renderUiScene()) renderer.displayBuffer();
        }
        if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      if (subActivity || M4PluginReaderSession::handoffBlocksLuaDisplay()) break;
      renderer.displayBuffer();
      if (host_.wantsExit()) exitRequested_.store(true, std::memory_order_relaxed);
      break;
    }
  }
}

void AppRuntimeActivity::runtimeTaskMain() {
  // Sole task allowed to enter host_/lua_State.
  handleEventOnOwner(M4xRuntime::Event::makeStart());

  uint32_t lastDrawMs = millis();
  bool sawStopHandler = false;

  while (!life_.isStopRequested() || !sawStopHandler) {
    M4xRuntime::Event e;
    const TickType_t wait =
        life_.isStopRequested() ? pdMS_TO_TICKS(10) : pdMS_TO_TICKS(kRecvTimeoutMs);
    const BaseType_t got = eventQueue_ ? xQueueReceive(eventQueue_, &e, wait) : pdFALSE;

    if (got == pdTRUE) {
      if (life_.shouldDrop(e.type)) continue;
      if (e.type == M4xRuntime::EventType::Stop) {
        handleEventOnOwner(e);
        sawStopHandler = true;
        break;
      }
      handleEventOnOwner(e);
      if (e.type == M4xRuntime::EventType::Draw || e.type == M4xRuntime::EventType::Key ||
          e.type == M4xRuntime::EventType::Touch || e.type == M4xRuntime::EventType::Tick) {
        lastDrawMs = millis();
      }
    } else if (!life_.isStopRequested()) {
      if (ready_.load(std::memory_order_relaxed) && !failed_.load(std::memory_order_relaxed)) {
        // Fast path while Lua dirty (loading pipeline) or provider prefetch work.
        // Otherwise idle 2s (still pumps history TOC restore while reader open).
        const bool providerBusy =
            subActivity && pluginChildKind_ == PluginChildKind::Reader &&
            M4ContentProviderSession::pendingWorkCount() > 0;
        const uint32_t idleMs =
            (host_.wantsPump() || providerBusy || host_.loaderNeedsPump()) ? kPumpIdleDrawMs : kIdleDrawMs;
        if (millis() - lastDrawMs >= idleMs) {
          handleEventOnOwner(M4xRuntime::Event::makeTick());
          lastDrawMs = millis();
        }
      }
    } else if (M4xRuntime::OwnerLifecycle::shouldInjectStop(life_.isStopRequested(), true)) {
      handleEventOnOwner(M4xRuntime::Event::makeStop());
      sawStopHandler = true;
      break;
    }
  }

  if (!life_.hostStopped) {
    host_.stop();
    life_.hostStopped = true;
  }
  ready_.store(false, std::memory_order_relaxed);
  life_.publishDone();
  vTaskDelete(nullptr);
}

void AppRuntimeActivity::requestStopAndJoin() {
  if (!ownerTaskStarted_) {
    if (!life_.isDone()) life_.publishDone();
    runtimeTask_ = nullptr;
    return;
  }

  life_.requestStop();
  host_.requestCancel();

  if (eventQueue_ && !life_.isDone()) {
    M4xRuntime::Event stop = M4xRuntime::Event::makeStop();
    if (xQueueSend(eventQueue_, &stop, pdMS_TO_TICKS(100)) != pdTRUE) {
      M4xRuntime::Event discard;
      xQueueReceive(eventQueue_, &discard, 0);
      xQueueSend(eventQueue_, &stop, pdMS_TO_TICKS(100));
    }
  }

  const uint32_t start = millis();
  bool warned = false;
  while (!life_.isDone()) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!warned && (millis() - start) > kJoinWarnMs) {
      warned = true;
      Serial.printf("[M4xRuntime] waiting for ownerDone (cancel in flight)...\n");
    }
  }

  runtimeTask_ = nullptr;
  ownerTaskStarted_ = false;
}

void AppRuntimeActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  life_.reset();
  failed_.store(false, std::memory_order_relaxed);
  exitRequested_.store(false, std::memory_order_relaxed);
  ready_.store(false, std::memory_order_relaxed);
  host_.clearCancel();
  errorBuf_[0] = '\0';
  runtimeTask_ = nullptr;
  ownerTaskStarted_ = false;
  childClosePending_ = false;
  M4PluginReaderSession::clearForApp(app_.id);
  M4ContentProviderSession::clearForApp(app_.id);

  errorMutex_ = xSemaphoreCreateMutex();
  eventQueue_ = xQueueCreate(kEventQueueLen, sizeof(M4xRuntime::Event));
  if (!eventQueue_ || !errorMutex_) {
    if (eventQueue_) {
      vQueueDelete(eventQueue_);
      eventQueue_ = nullptr;
    }
    if (errorMutex_) {
      vSemaphoreDelete(errorMutex_);
      errorMutex_ = nullptr;
    }
    life_.publishDone();
    setFailed("runtime_queue_or_mutex");
    renderError();
    return;
  }

  const BaseType_t ok =
      xTaskCreate(&AppRuntimeActivity::taskTrampoline, "M4xRuntime", 12288, this, 1, &runtimeTask_);
  if (ok != pdPASS) {
    runtimeTask_ = nullptr;
    ownerTaskStarted_ = false;
    vQueueDelete(eventQueue_);
    eventQueue_ = nullptr;
    vSemaphoreDelete(errorMutex_);
    errorMutex_ = nullptr;
    life_.publishDone();
    setFailed("runtime_task_create");
    renderError();
    return;
  }
  ownerTaskStarted_ = true;
}

void AppRuntimeActivity::onExit() {
  // Tear down child first (parent frame, not from child callback).
  exitActivity();
  ActivityWithSubactivity::onExit();
  M4PluginReaderSession::clearForApp(app_.id);
  M4ContentProviderSession::clearForApp(app_.id);
  requestStopAndJoin();
  if (eventQueue_) {
    vQueueDelete(eventQueue_);
    eventQueue_ = nullptr;
  }
  if (errorMutex_) {
    vSemaphoreDelete(errorMutex_);
    errorMutex_ = nullptr;
  }
}

void AppRuntimeActivity::tryLaunchPluginReader() {
  // Owner-task only. UI loop must never call this (e-ink race with display task).
  if (subActivity) return;
  M4PluginReaderBridge::OpenRequest req;
  if (!M4PluginReaderSession::takeOpen(req)) return;
  if (req.appId != app_.id) {
    // Mismatched app: drop and report via progress error channel.
    Serial.printf("[WR05] t=%lu drop_open app=%s this=%s\n", static_cast<unsigned long>(millis()),
                  req.appId.c_str(), app_.id.c_str());
    M4PluginReaderSession::clearPendingOpen();
    return;
  }

  Serial.printf("[WR05] t=%lu launch_begin gen=%u path=%s\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned>(req.generation), req.relPath.c_str());
  if (req.pendingComplete) {
    // Loader early-open: the chapter body is still streaming into the file.
    // Show a loading placeholder instead of paginating the partial body; a
    // second open (pendingComplete=false) arrives when the body is complete.
    M4PluginReaderSession::clearLaunchInProgress();
    renderer.clearScreen();
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 220, "加载中…", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 280, req.title.c_str());
    renderer.displayBuffer();
    postEvent(M4xRuntime::Event::makeDraw());
    return;
  }
  const uint32_t handoffStartedMs = millis();
  // Free the plugin's keep-alive TLS session before the native reader runs —
  // ~32KB internal RAM that otherwise makes the next chapter open OOM. The
  // reader itself does no networking; provider next-chapter prefetch builds a
  // fresh handshake, which succeeds when internal RAM is calm (and retries
  // otherwise).
  host_.releaseNetworkSession();
  const uint32_t tLoad0 = millis();
  auto txt = std::make_unique<Txt>(req.absPath, "/.crosspoint");
  const uint32_t tLoadStart = millis();
  const bool loaded = txt->load();
  const uint32_t tLoadMs = millis() - tLoadStart;
  Serial.printf("[WR05] t=%lu txt_load ok=%d ms=%lu size=%u enc_ok=%d\n", static_cast<unsigned long>(millis()),
                loaded ? 1 : 0, static_cast<unsigned long>(tLoadMs),
                loaded ? static_cast<unsigned>(txt->getFileSize()) : 0u,
                (loaded && txt->isEncodingSupported()) ? 1 : 0);
  // Diagnostic to SD (serial unreliable): a slow Txt::load on the main loop is
  // the classic "open book → watchdog reboot" — long blocking SD scan.
  {
    FsFile df = SdMan.open("apps_data/com.jjwxc.client/logs/reader_open.log",
                           O_WRONLY | O_CREAT | O_APPEND);
    if (df) {
      char line[160];
      const int n = snprintf(line, sizeof(line),
                             "[%lu] open path=%s load_ms=%lu ok=%d size=%u free=%u\n",
                             (unsigned long)millis(), req.relPath.c_str(), (unsigned long)tLoadMs,
                             loaded ? 1 : 0,
                             loaded ? (unsigned)txt->getFileSize() : 0u,
                             (unsigned)ESP.getFreeHeap());
      if (n > 0) df.write(reinterpret_cast<const uint8_t*>(line), (size_t)n);
      df.close();
    }
  }
  if (!loaded || !txt->isEncodingSupported()) {
    // Explicit open failure — never looks like a successful close at page 1.
    M4PluginReaderSession::ProgressSnapshot snap;
    snap.page = 0;
    snap.total = -1;
    snap.complete = false;
    snap.openFailed = true;
    snap.generation = req.generation;
    std::strncpy(snap.error, !loaded ? "load_failed" : "encoding_unsupported", sizeof(snap.error) - 1);
    std::strncpy(snap.bookId, req.bookId.c_str(), sizeof(snap.bookId) - 1);
    std::strncpy(snap.chapterUid, req.chapterUid.c_str(), sizeof(snap.chapterUid) - 1);
    std::strncpy(snap.progressKey, req.progressKey.c_str(), sizeof(snap.progressKey) - 1);
    M4PluginReaderSession::publishProgress(snap);
    M4PluginReaderSession::clearLaunchInProgress();
    postEvent(M4xRuntime::Event::makeDraw());
    return;
  }

  TxtReaderActivity::PluginSession sess;
  sess.active = true;
  sess.suppressRecentBooks = true;
  sess.suppressOpenEpubPath = true;
  sess.progressiveIndex = true;
  sess.bookId = req.bookId;
  sess.chapterUid = req.chapterUid;
  sess.progressKey = req.progressKey;
  sess.titleOverride = req.title;
  sess.generation = req.generation;
  if (req.hasInitialByteOffset) {
    const size_t fsz = txt->getFileSize();
    size_t off = static_cast<size_t>(req.initialByteOffset);
    if (fsz == 0) {
      off = 0;
    } else if (off >= fsz) {
      off = fsz - 1;
    }
    sess.initialByteOffset = off;
    sess.hasInitialByteOffset = true;
  }

  childClosePending_ = false;
  pluginChildKind_ = PluginChildKind::Reader;
  sess.tocRelPath = req.tocRelPath;
  sess.tocAbsPath = req.tocAbsPath;
  sess.chapterIndex = req.chapterIndex;
  sess.providerId = req.providerId;
  sess.appId = app_.id;  // reverse-DNS package id — history author field (never providerId)
  sess.appDataRoot = host_.dataDir();
  sess.cacheRelPath = req.relPath;
  sess.providerManaged = !req.providerId.empty();
  if (sess.providerManaged) {
    // Provider books register in history (URI identity) after first open.
    sess.suppressRecentBooks = false;
  }
  enterNewActivity(new TxtReaderActivity(
      renderer, mappedInput, std::move(txt),
      [this]() {
        // Child onGoBack: only signal parent — do NOT exitActivity here.
        childClosePending_ = true;
      },
      [this]() {
        childClosePending_ = true;
      },
      std::move(sess)));
  M4PluginReaderSession::clearLaunchInProgress();
  Serial.printf("[WR05] t=%lu launch_entered gen=%u progressive=1\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned>(req.generation));
  Serial.printf("[WRPERF] stage=native_handoff ms=%lu gen=%u\n",
                static_cast<unsigned long>(millis() - handoffStartedMs),
                static_cast<unsigned>(req.generation));
}

void AppRuntimeActivity::tryLaunchPluginToc() {
  // Owner-task only. Same e-ink race rules as tryLaunchPluginReader.
  if (subActivity) return;
  M4PluginReaderSession::TocRequest req;
  if (!M4PluginReaderSession::takeToc(req)) return;
  if (req.appId != app_.id) {
    Serial.printf("[WR05] t=%lu drop_toc app=%s this=%s\n", static_cast<unsigned long>(millis()),
                  req.appId.c_str(), app_.id.c_str());
    M4PluginReaderSession::clearPendingToc();
    return;
  }

  Serial.printf("[WR05] t=%lu toc_launch_begin gen=%u path=%s\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned>(req.generation), req.tocRelPath.c_str());

  std::vector<std::string> titles;
  std::shared_ptr<M4PluginTocList::PagedTitleSource> pagedTitles;
  bool loadedTitles = false;
  if (!req.tocAbsPath.empty()) {
    loadedTitles = M4PluginTocList::loadTitlesFromFile(req.tocAbsPath, titles);
  } else if (!req.providerId.empty() && !req.appDataRoot.empty()) {
    // Provider-managed catalogs are opened as a bounded page source.  The
    // native picker owns the system layout and asks for only its visible rows;
    // no catalog-sized title vector is copied into the owner task or display.
    M4ContentProvider::ChapterCatalogSpec catalog;
    std::string absPath;
    if (M4ContentProviderSession::catalogFor(req.providerId, req.bookId, req.currentIndex, catalog) &&
        M4PluginReaderBridge::resolveUnderDataRoot(req.appDataRoot, catalog.fileRelPath.c_str(), absPath) ==
            M4PluginReaderBridge::OpenError::Ok) {
      pagedTitles = M4PluginTocList::openPagedFileRows(absPath, catalog);
      loadedTitles = static_cast<bool>(pagedTitles);
    }
  }
  const int chapterCount = pagedTitles ? static_cast<int>(pagedTitles->rowCount()) : static_cast<int>(titles.size());
  if (!loadedTitles || chapterCount <= 0) {
    M4PluginReaderSession::TocResult r;
    r.cancelled = true;
    r.chapterIndex = -1;
    r.generation = req.generation;
    std::strncpy(r.bookId, req.bookId.c_str(), sizeof(r.bookId) - 1);
    M4PluginReaderSession::publishTocResult(r);
    M4PluginReaderSession::clearLaunchInProgress();
    postEvent(M4xRuntime::Event::makeDraw());
    Serial.printf("[WR05] t=%lu toc_launch_fail empty\n", static_cast<unsigned long>(millis()));
    return;
  }

  int cur = req.currentIndex;
  if (cur < 0) cur = 0;
  if (cur >= chapterCount) cur = chapterCount - 1;

  childClosePending_ = false;
  pluginChildKind_ = PluginChildKind::Toc;
  tocBookId_ = req.bookId;
  tocGeneration_ = req.generation;
  const std::string header = req.bookTitle.empty() ? std::string("目  录") : req.bookTitle;
  if (pagedTitles) {
    auto loader = [pagedTitles](int first, int count, std::vector<std::string>& pageTitles,
                                std::vector<uint8_t>& pagePresent) {
      return pagedTitles->loadPage(first, count, pageTitles, pagePresent);
    };
    enterNewActivity(new TxtReaderChapterSelectionActivity(
        renderer, mappedInput, chapterCount, std::move(loader), cur,
        [this]() {
          // Back: signal parent; do NOT exitActivity from inside child.
          childClosePending_ = true;
        },
        [this](int /*idx*/) {
          // Select: same two-phase close; index read from finished()/selectedIndex().
          childClosePending_ = true;
        },
        header));
  } else {
    enterNewActivity(new TxtReaderChapterSelectionActivity(
        renderer, mappedInput, std::move(titles), cur,
        [this]() {
          childClosePending_ = true;
        },
        [this](int /*idx*/) {
          childClosePending_ = true;
        },
        header));
  }
  M4PluginReaderSession::clearLaunchInProgress();
  Serial.printf("[WR05] t=%lu toc_launch_entered gen=%u titles=%d paged=%d cur=%d\n",
                static_cast<unsigned long>(millis()), static_cast<unsigned>(req.generation), chapterCount,
                pagedTitles ? 1 : 0, cur);
}

void AppRuntimeActivity::loop() {
  // Native reader / TOC as sub-activity: two-phase close.
  if (subActivity) {
    subActivity->loop();
    // After child frame returns, apply deferred close.
    if (childClosePending_) {
      childClosePending_ = false;
      if (pluginChildKind_ == PluginChildKind::Reader) {
        auto* reader = static_cast<TxtReaderActivity*>(subActivity.get());
        if (reader && reader->pluginCloseRequested()) {
          // May block until current render/index slice releases the state lock.
          const auto p = reader->pluginProgressSnapshot();
          M4PluginReaderSession::ProgressSnapshot snap;
          if (!p.valid) {
            // Explicit failure — Lua must not persist a fabricated page-1 snapshot.
            snap.openFailed = true;
            snap.generation = p.generation;
            std::strncpy(snap.error, "snapshot_unavailable", sizeof(snap.error) - 1);
            std::strncpy(snap.bookId, p.bookId.c_str(), sizeof(snap.bookId) - 1);
            std::strncpy(snap.chapterUid, p.chapterUid.c_str(), sizeof(snap.chapterUid) - 1);
            std::strncpy(snap.progressKey, p.progressKey.c_str(), sizeof(snap.progressKey) - 1);
            snap.switchChapterIndex = p.switchChapterIndex;
          } else {
            snap.page = p.page;
            snap.total = p.total;
            snap.byteOffset = p.byteOffset;
            snap.complete = p.indexComplete;
            // Exact reader session generation — do not substitute currentGeneration().
            snap.generation = p.generation;
            std::strncpy(snap.bookId, p.bookId.c_str(), sizeof(snap.bookId) - 1);
            std::strncpy(snap.chapterUid, p.chapterUid.c_str(), sizeof(snap.chapterUid) - 1);
            std::strncpy(snap.progressKey, p.progressKey.c_str(), sizeof(snap.progressKey) - 1);
            snap.switchChapterIndex = p.switchChapterIndex;
          }
          M4PluginReaderSession::publishProgress(snap);
        }
      } else if (pluginChildKind_ == PluginChildKind::Toc) {
        auto* toc = static_cast<TxtReaderChapterSelectionActivity*>(subActivity.get());
        M4PluginReaderSession::TocResult r;
        r.generation = tocGeneration_;
        std::strncpy(r.bookId, tocBookId_.c_str(), sizeof(r.bookId) - 1);
        if (toc && toc->finished() && !toc->cancelled() && toc->selectedIndex() >= 0) {
          r.cancelled = false;
          r.chapterIndex = toc->selectedIndex();
        } else {
          r.cancelled = true;
          r.chapterIndex = -1;
        }
        M4PluginReaderSession::publishTocResult(r);
      }
      pluginChildKind_ = PluginChildKind::None;
      exitActivity();  // safe: child loop already returned
      postEvent(M4xRuntime::Event::makeDraw());
    }
    return;
  }

  // Do NOT tryLaunchPluginReader here. Launch is owner-task only (after draw).
  // Dual launch raced e-ink SPI/BUSY with the TxtReader display task.

  if (exitRequested_.load(std::memory_order_relaxed) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onExitApp_();
    return;
  }

  if (failed_.load(std::memory_order_relaxed)) {
    int tx = 0, ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      onExitApp_();
    }
    return;
  }

  if (!ready_.load(std::memory_order_relaxed) || life_.isStopRequested()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    postEvent(M4xRuntime::Event::makeKey("confirm"));
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    postEvent(M4xRuntime::Event::makeKey("up"));
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    postEvent(M4xRuntime::Event::makeKey("down"));
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    postEvent(M4xRuntime::Event::makeKey("left"));
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    postEvent(M4xRuntime::Event::makeKey("right"));
  }

  int tx = 0, ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    postEvent(M4xRuntime::Event::makeTouch(tx, ty, "tap"));
  }
}

void AppRuntimeActivity::renderError() {
  std::string err;
  copyError(err);
  std::vector<std::string> diag;
  M4ErrorScreen::appendCode(diag, err);
  M4ErrorScreen::addKV(diag, "app_id: ", app_.id);
  M4ErrorScreen::addKV(diag, "app_name: ", app_.name);
  M4ErrorScreen::addKV(diag, "provider: ", app_.provider);
  M4ErrorScreen::addKV(diag, "path: ", app_.path);
  M4ErrorScreen::addKV(diag, "entry: ", app_.entry);
  const char* back = M4InputProfile::showHardwareKeyHints() ? "« 返回" : "tap exit";
  auto snap = M4ErrorScreen::genericFail(app_.name.empty() ? "应用" : app_.name, "应用运行失败", err, diag,
                                         back, "");
  snap.fastRefresh = false;
  M4ErrorScreen::paint(renderer, snap, true);
}

std::string AppRuntimeActivity::debugUiJson() {
  std::string err;
  copyError(err);
  std::string out = "{\"kind\":\"app_runtime\",\"app_id\":\"";
  out += app_.id;
  out += "\",\"app_name\":\"";
  out += app_.name;
  out += "\",\"failed\":";
  out += failed_.load(std::memory_order_relaxed) ? "true" : "false";
  out += ",\"ready\":";
  out += ready_.load(std::memory_order_relaxed) ? "true" : "false";
  out += ",\"error\":\"";
  for (unsigned char c : err) {
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
  out += "\",\"plugin\":";
  // Nested plugin dump (already a JSON object).
  out += host_.debugUiJson();
  out += '}';
  return out;
}

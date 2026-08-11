#include "ReaderActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "GbkToUtf8.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "TxtToEpubConverter.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4IndexingState.h"
#include "util/M4ReaderOpenDispatch.h"
#include "util/M4HistoryReopen.h"
#include "util/M4ContentProviderContract.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/M4xRegistry.h"
#include "apps/M4xPaths.h"
#include "activities/apps/AppRuntimeActivity.h"
#include "activities/apps/NativeProviderBookActivity.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "RecentBooksStore.h"
#include "util/StringUtils.h"

#include <SDCardManager.h>

#include <cstring>

namespace {
void indexTerminalReady(const char* msg) {
  auto& m = M4IndexingState::Session::get().machine();
  M4ReaderOpenDispatch::onOpenSuccess(m, msg);
  Serial.printf("[%lu] %s\n", millis(), M4IndexingState::formatLogLine(m.snap()).c_str());
}

void indexTerminalFail(const char* msg) {
  auto& m = M4IndexingState::Session::get().machine();
  M4ReaderOpenDispatch::onOpenFail(m, msg);
  Serial.printf("[%lu] %s\n", millis(), M4IndexingState::formatLogLine(m.snap()).c_str());
}
}  // namespace

std::string ReaderActivity::extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

bool ReaderActivity::isXtcFile(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch");
}

bool ReaderActivity::isTxtFile(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".txt") ||
         StringUtils::checkFileExtension(path, ".md");  // Treat .md as txt files (until we have a markdown reader)
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [   ] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (epub->load()) {
    return epub;
  }

  Serial.printf("[%lu] [   ] Failed to load epub\n", millis());
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [   ] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (xtc->load()) {
    return xtc;
  }

  Serial.printf("[%lu] [   ] Failed to load XTC\n", millis());
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!SdMan.exists(path.c_str())) {
    Serial.printf("[%lu] [   ] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint"));
  if (txt->load()) {
    return txt;
  }

  Serial.printf("[%lu] [   ] Failed to load TXT\n", millis());
  return nullptr;
}

// True only when non-direct path would still want a legacy whole-file UTF-8 cache
// popup. Direct TXT read never full-converts; streaming decode handles GBK/UTF-16.
bool ReaderActivity::needsGbkToUtf8Conversion(const std::string& path) {
  // Direct-read path: never whole-book convert / never show convert popup.
  if (SETTINGS.directTxtRead) {
    return false;
  }
  // TXT→EPUB path uses TxtToEpubConverter (separate); no GBK sidecar required.
  (void)path;
  return false;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  const auto initialPath = fromBookPath.empty() ? "/" : extractFolderPath(fromBookPath);
  onGoToLibrary(initialPath);
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  indexTerminalReady("epub_open");
  exitActivity();
  // 如果有 originalSourcePath（从最近阅读或睡眠恢复时传入），则传递给 EpubReaderActivity
  enterNewActivity(new EpubReaderActivity(
      renderer, mappedInput, std::move(epub), [this, epubPath] { goToLibrary(epubPath); }, [this] { onGoBack(); },
      originalSourcePath));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  indexTerminalReady("xtc_open");
  exitActivity();
  enterNewActivity(new XtcReaderActivity(
      renderer, mappedInput, std::move(xtc), [this, xtcPath] { goToLibrary(xtcPath); }, [this] { onGoBack(); }));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  constexpr auto cacheDir = "/.crosspoint";

  // Unsupported encoding (e.g. GB18030 4-byte / Unknown): do not open as UTF-8 garbage.
  if (txt && !txt->isEncodingSupported()) {
    const char* diag = txt->getEncodingDiagnostic();
    Serial.printf("[%lu] [RDR] TXT encoding unsupported path=%s enc=%s diag=%s\n", millis(), txtPath.c_str(),
                  txt->getEncodingName(), diag && diag[0] ? diag : "-");
    indexTerminalFail("txt_encoding_unsupported");
    std::string msg = "不支持的文本编码";
    if (diag && std::strstr(diag, "gb18030") != nullptr) {
      msg = "不支持 GB18030 四字节编码\n请转换为 UTF-8 或 GBK";
    } else if (diag && diag[0]) {
      msg = std::string("不支持的文本编码\n") + diag;
    }
    renderer.clearScreen();
    GUI.drawPopup(renderer, msg.c_str());
    renderer.displayBuffer();
    // Brief hold then return — avoid silent mojibake
    delay(1500);
    onGoBack();
    return;
  }

  // 根据设置决定是直接读取TXT还是转换为EPUB
  if (SETTINGS.directTxtRead) {
    // 直读TXT模式 — must clear Preparing from onEnter and reach Ready.
    currentBookPath = txtPath;
    indexTerminalReady("direct_txt");
    exitActivity();
    enterNewActivity(new TxtReaderActivity(
        renderer, mappedInput, std::move(txt),
        [this, txtPath] { goToLibrary(txtPath); },
        [this] { onGoBack(); }));
    return;
  }

  // 转换为EPUB模式
  // Check cache validity; convert if needed
  if (!TxtToEpubConverter::isCacheValid(txtPath, cacheDir)) {
    const Rect popupRect = GUI.drawPopup(renderer, "正在转换TXT...");
    renderer.displayBuffer();
    int lastProgressPct = -1;
    auto& idx = M4IndexingState::Session::get().machine();
    idx.reset();
    idx.beginPreparing("txt_to_epub");
    idx.beginIndexing(100, "txt_to_epub");
    {
      M4IndexingState::ScopeGuard guard("txt_to_epub_aborted");
      const uint32_t t0 = millis();
      if (!TxtToEpubConverter::convert(txtPath, cacheDir, [&](int pct) {
            if (pct - lastProgressPct >= 5 || pct >= 100) {
              GUI.fillPopupProgress(renderer, popupRect, pct);
              if (M4IndexingState::shouldRefreshProgressUi(lastProgressPct, pct)) {
                renderer.displayBuffer();
              }
              lastProgressPct = pct;
              idx.reportProgress(pct, static_cast<uint32_t>(pct), millis() - t0, "txt_to_epub");
              Serial.printf("[%lu] %s\n", millis(), M4IndexingState::formatLogLine(idx.snap()).c_str());
            }
          })) {
        Serial.printf("[%lu] [RDR] TXT->EPUB conversion failed, falling back\n", millis());
        idx.fail(millis() - t0, "txt_to_epub");
        Serial.printf("[%lu] %s\n", millis(), M4IndexingState::formatLogLine(idx.snap()).c_str());
        guard.markSucceeded();
        renderer.clearScreen();
        renderer.displayBuffer();
        currentBookPath = txtPath;
        exitActivity();
        enterNewActivity(new TxtReaderActivity(
            renderer, mappedInput, std::move(txt),
            [this, txtPath] { goToLibrary(txtPath); }, [this] { onGoBack(); }));
        return;
      }
      idx.succeed(millis() - t0, "txt_to_epub");
      Serial.printf("[%lu] %s\n", millis(), M4IndexingState::formatLogLine(idx.snap()).c_str());
      guard.markSucceeded();
    }
  }

  // Load the cached EPUB
  const auto epubPath = TxtToEpubConverter::getCachedEpubPath(txtPath, cacheDir);
  auto epub = loadEpub(epubPath);
  if (!epub) {
    // EPUB load failed — clear loading popup state and fall back to TXT reader
    Serial.printf("[%lu] [RDR] Cached EPUB load failed, falling back\n", millis());
    {
      auto& idx = M4IndexingState::Session::get().machine();
      if (idx.snap().busy || idx.snap().phase == M4IndexingState::Phase::Preparing) {
        idx.fail(0, "cached_epub_load");
      } else {
        idx.fail(0, "cached_epub_load");
      }
      Serial.printf("[%lu] %s\n", millis(), M4IndexingState::formatLogLine(idx.snap()).c_str());
    }
    renderer.clearScreen();
    renderer.displayBuffer();
    currentBookPath = txtPath;
    exitActivity();
    enterNewActivity(new TxtReaderActivity(
        renderer, mappedInput, std::move(txt),
        [this, txtPath] { goToLibrary(txtPath); }, [this] { onGoBack(); }));
    return;
  }

  // Open EpubReaderActivity with the converted book
  currentBookPath = txtPath;
  indexTerminalReady("txt_via_epub");
  exitActivity();
  enterNewActivity(new EpubReaderActivity(
      renderer, mappedInput, std::move(epub),
      [this, txtPath] { goToLibrary(txtPath); }, [this] { onGoBack(); }, txtPath));
}

// Load TXT for reading. Direct path: stream-decode only (no whole-file UTF-8 copy,
// no convert popup). Explicit TXT→EPUB is handled in onGoToTxtReader when
// directTxtRead is off.
std::unique_ptr<Txt> ReaderActivity::loadTxtWithConversion(const std::string& path) {
  if (needsGbkToUtf8Conversion(path)) {
    // Reserved: never true while streaming policy is active.
    const Rect popupRect = GUI.drawPopup(renderer, "正在转换为UTF-8编码...");
    (void)popupRect;
    renderer.displayBuffer();
  }

  auto txt = loadTxt(path);

  if (txt && needsGbkToUtf8Conversion(path)) {
    renderer.clearScreen();
    renderer.displayBuffer();
  }

  return txt;
}

void ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  // History / Recent Books path: m4cp://provider/book vs legacy SD paths.
  //
  // Contract (cold reopen):
  // 1. Always launch AppRuntime for provider books when the m4x app is installed.
  //    Direct TxtReader has no owner for system-TOC chapter switch → desktop crash.
  // 2. Queue HistoryResume with last chapterUid/cacheRelPath so the plugin opens
  //    the cached body immediately (no login/session flash when cache exists).
  // 3. Session/TOC network refresh happens in the background after native open.
  {
    auto exists = [](const std::string& p) -> bool { return SdMan.exists(p.c_str()); };
    auto appInstalled = [](const std::string& id) -> bool {
      const auto apps = M4xRegistry::load();
      return M4xRegistry::find(apps, id) != nullptr;
    };
    // Recover title + author(appId) from RecentBooksStore. Callbacks only pass
    // path/originalSourcePath; author holds reverse-DNS app id for m4cp entries.
    std::string historyTitle;
    std::string authorAppId;
    for (const auto& b : RECENT_BOOKS.getBooks()) {
      if (b.path == initialBookPath) {
        historyTitle = b.title;
        authorAppId = b.author;
        break;
      }
    }
    auto hist = M4HistoryReopen::resolveFromRecentBookFields(
        initialBookPath, originalSourcePath, historyTitle, authorAppId, exists, appInstalled);

    // Prefer in-session reopen metadata when still warm (same boot).
    if (M4ContentProvider::isHistoryUri(initialBookPath.c_str())) {
      std::string pid, bid;
      if (M4ContentProvider::parseHistoryUri(initialBookPath.c_str(), pid, bid)) {
        M4ContentProvider::ChapterStatus st;
        if (M4ContentProviderSession::resolveReopen(pid, bid, st) &&
            st.state == M4ContentProvider::ChapterReady::Ready && !st.cacheRelPath.empty()) {
          if (hist.chapterUid.empty()) hist.chapterUid = st.chapterUid;
          if (hist.cacheRelPath.empty()) hist.cacheRelPath = st.cacheRelPath;
          if (hist.appId.empty() && M4HistoryReopen::looksLikeAppId(authorAppId)) {
            hist.appId = authorAppId;
            hist.appDataRoot = M4HistoryReopen::appDataRootFor(hist.appId);
          }
          if (!hist.appId.empty() && hist.openPath.empty()) {
            hist.openPath = hist.appDataRoot + "/" + st.cacheRelPath;
            if (exists(hist.openPath)) hist.kind = M4HistoryReopen::Kind::ProviderCached;
          }
        }
      }
    }

    // Provider books with an installed app ALWAYS go through AppRuntime so
    // chapter-list switch has an owner. Plugin opens cached chapter first.
    if ((hist.kind == M4HistoryReopen::Kind::ProviderCached ||
         hist.kind == M4HistoryReopen::Kind::ProviderNeedsFetch)) {
      std::string appId = hist.appId;
      if (appId.empty() && originalSourcePath.compare(0, 4, "app:") == 0) {
        const std::string cand = originalSourcePath.substr(4);
        if (M4HistoryReopen::looksLikeAppId(cand)) appId = cand;
      }
      if (appId.empty() && M4HistoryReopen::looksLikeAppId(authorAppId)) {
        appId = authorAppId;
      }
      if (!appId.empty() && appInstalled(appId)) {
        M4ContentProviderSession::HistoryResume resume;
        resume.appId = appId;
        resume.providerId = hist.providerId;
        resume.bookId = hist.bookId;
        resume.title = hist.title;
        resume.chapterUid = hist.chapterUid;
        resume.cacheRelPath = hist.cacheRelPath;
        resume.chapterIndex0 = -1;  // unknown unless session has it
        {
          auto snap = M4ContentProviderSession::makeHistorySnapshot(hist.providerId, hist.bookId);
          if (!snap.chapterUid.empty() &&
              (resume.chapterUid.empty() || snap.chapterUid == resume.chapterUid)) {
            if (resume.chapterUid.empty()) resume.chapterUid = snap.chapterUid;
            if (resume.cacheRelPath.empty()) resume.cacheRelPath = snap.cacheRelPath;
            resume.chapterIndex0 = snap.chapterIndex0;
            if (snap.byteOffset > 0) {
              resume.byteOffset = snap.byteOffset;
              resume.hasByteOffset = true;
            }
          } else if (snap.chapterIndex0 >= 0 && resume.chapterUid.empty()) {
            resume.chapterIndex0 = snap.chapterIndex0;
          }
        }
        const auto apps = M4xRegistry::load();
        if (const auto* app = M4xRegistry::find(apps, appId)) {
          // Native provider apps must not be sent through AppRuntimeActivity:
          // that activity starts the Lua VM and reports a runtime failure for
          // a native main.xml package. Reopen the cached chapter directly and
          // keep the native provider session alive for next-chapter switching.
          if (app->runtime == M4xRuntimeKind::Native &&
              M4NativeProviderManager::supports(hist.providerId)) {
            const bool bookReady = M4NativeProviderManager::ensureBook(
                hist.providerId, hist.bookId, appId, hist.title);
            int chapterIndex = resume.chapterIndex0;
            if (bookReady && chapterIndex < 0 && !resume.chapterUid.empty()) {
              (void)M4NativeProviderManager::findChapterIndex(
                  hist.providerId, hist.bookId, resume.chapterUid, chapterIndex);
            }
            if (bookReady && chapterIndex < 0) chapterIndex = 0;

            const std::string appDataRoot = hist.appDataRoot.empty()
                                                ? M4NativeProviderManager::appDataRootFor(hist.providerId,
                                                                                           hist.bookId)
                                                : hist.appDataRoot;
            const std::string cacheRelPath = !resume.cacheRelPath.empty()
                                                 ? resume.cacheRelPath
                                                 : hist.cacheRelPath;
            const std::string cacheAbsPath = !hist.openPath.empty()
                                                 ? hist.openPath
                                                 : (appDataRoot.empty() || cacheRelPath.empty()
                                                        ? std::string()
                                                        : appDataRoot + "/" + cacheRelPath);
            auto txt = cacheAbsPath.empty() ? std::unique_ptr<Txt>() : loadTxt(cacheAbsPath);
            if (bookReady && txt && txt->isEncodingSupported() && txt->getFileSize() > 0 &&
                chapterIndex >= 0) {
              const std::string chapterUid = !resume.chapterUid.empty() ? resume.chapterUid : hist.chapterUid;
              M4ContentProvider::ChapterStatus ready;
              ready.providerId = hist.providerId;
              ready.bookId = hist.bookId;
              ready.chapterUid = chapterUid;
              ready.index0 = chapterIndex;
              ready.state = M4ContentProvider::ChapterReady::Ready;
              ready.cacheRelPath = cacheRelPath;
              (void)M4ContentProviderSession::setChapterStatus(ready);
              M4ContentProviderSession::noteOpen(hist.providerId, hist.bookId, chapterIndex,
                                                 resume.hasByteOffset ? resume.byteOffset : 0);

              TxtReaderActivity::PluginSession sess;
              sess.active = true;
              sess.suppressRecentBooks = false;
              sess.suppressOpenEpubPath = true;
              sess.progressiveIndex = true;
              sess.bookId = hist.bookId;
              sess.chapterUid = chapterUid;
              sess.progressKey = hist.providerId + ":" + hist.bookId + ":" + chapterUid;
              sess.titleOverride = hist.title;
              sess.chapterIndex = chapterIndex;
              sess.providerManaged = true;
              sess.providerId = hist.providerId;
              sess.appId = appId;
              sess.appDataRoot = appDataRoot;
              sess.cacheRelPath = cacheRelPath;
              if (resume.hasByteOffset) {
                sess.initialByteOffset = resume.byteOffset;
                sess.hasInitialByteOffset = true;
              }
              indexTerminalReady("provider_history_native");
              Serial.printf("[WRCP] history_open app=%s book=%s ch=%s idx=%d cache=%s via=native\n",
                            appId.c_str(), hist.bookId.c_str(), chapterUid.c_str(), chapterIndex,
                            cacheRelPath.c_str());
              renderer.clearScreen();
              M4UiText::drawCentered(renderer, UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                                     "打开上次阅读…", true, EpdFontFamily::BOLD);
              renderer.displayBuffer(HalDisplay::FAST_REFRESH);
              currentBookPath = cacheAbsPath;
              exitActivity();
              enterNewActivity(new TxtReaderActivity(
                  renderer, mappedInput, std::move(txt), [this] { onGoBack(); }, [this] { onGoBack(); },
                  std::move(sess)));
              return;
            }

            // No usable cached body: stay native and let the provider-owned
            // book activity show its TOC/loading/error state instead of
            // falling into the Lua runtime.
            indexTerminalReady("provider_history_native_book");
            exitActivity();
            enterNewActivity(new NativeProviderBookActivity(
                renderer, mappedInput, hist.providerId, hist.bookId, appId, hist.title,
                [this] { onGoBack(); }));
            return;
          }

          // Lua providers still consume the one-shot resume handoff in their
          // existing AppRuntimeActivity path.
          M4ContentProviderSession::queueHistoryResume(resume);
          indexTerminalReady("provider_history_app");
          Serial.printf(
              "[WRCP] history_open app=%s book=%s ch=%s cache=%s via=runtime\n", appId.c_str(),
              hist.bookId.c_str(), resume.chapterUid.c_str(), resume.cacheRelPath.c_str());
          // Brief non-blocking notice (one FAST frame) then hand off to app runtime.
          // Plugin skips login and opens cached chapter when body is on SD.
          renderer.clearScreen();
          const char* msg = !resume.cacheRelPath.empty()
                                ? "打开上次阅读…"
                                : (hist.overlayMessage.empty() ? "打开内容提供方…"
                                                               : hist.overlayMessage.c_str());
          M4UiText::drawCentered(renderer, UI_12_FONT_ID, renderer.getScreenHeight() / 2, msg, true,
                                    EpdFontFamily::BOLD);
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          exitActivity();
          enterNewActivity(new AppRuntimeActivity(renderer, mappedInput, *app, [this] { onGoBack(); }));
          return;
        }
      }
      // App missing or unresolved: only when we still have a readable cache, open
      // native-only as last resort (no chapter switch). Otherwise fail cleanly.
      if (hist.kind == M4HistoryReopen::Kind::ProviderCached && !hist.openPath.empty() &&
          exists(hist.openPath)) {
        auto txt = loadTxt(hist.openPath);
        if (txt && txt->isEncodingSupported()) {
          indexTerminalReady("provider_history_cache_fallback");
          TxtReaderActivity::PluginSession sess;
          sess.active = true;
          sess.providerManaged = true;
          sess.providerId = hist.providerId;
          sess.bookId = hist.bookId;
          sess.chapterUid = hist.chapterUid;
          sess.cacheRelPath = hist.cacheRelPath;
          sess.appDataRoot = hist.appDataRoot;
          sess.appId = appId;
          sess.titleOverride = hist.title;
          sess.tocRelPath = "cache/" + hist.bookId + "/toc.json";
          if (!sess.appDataRoot.empty()) {
            sess.tocAbsPath = sess.appDataRoot + "/" + sess.tocRelPath;
          }
          sess.suppressOpenEpubPath = true;
          sess.suppressRecentBooks = false;
          sess.progressiveIndex = true;
          sess.progressKey = hist.providerId + ":" + hist.bookId + ":" + hist.chapterUid;
          currentBookPath = hist.openPath;
          exitActivity();
          enterNewActivity(new TxtReaderActivity(
              renderer, mappedInput, std::move(txt), [this] { onGoBack(); }, [this] { onGoBack(); },
              std::move(sess)));
          return;
        }
      }
      indexTerminalFail("provider_history_unavailable");
      renderer.clearScreen();
      M4UiText::drawCentered(renderer, UI_12_FONT_ID, renderer.getScreenHeight() / 2, "无法打开：章节未缓存", true,
                                EpdFontFamily::BOLD);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      delay(1200);
      onGoBack();
      return;
    }
    // LocalFile / Invalid: fall through with filesystem path.
  }

  // Show loading indicator only on first open (cache directory does not exist yet)
  {
    constexpr auto cacheBase = "/.crosspoint";
    const size_t hashVal = std::hash<std::string>{}(initialBookPath);
    std::string bookCacheDir;
    if (isXtcFile(initialBookPath)) {
      bookCacheDir = std::string(cacheBase) + "/xtc_" + std::to_string(hashVal);
    } else if (isTxtFile(initialBookPath)) {
      bookCacheDir = std::string(cacheBase) + "/txt_" + std::to_string(hashVal);
    } else {
      bookCacheDir = std::string(cacheBase) + "/epub_" + std::to_string(hashVal);
    }
    if (!SdMan.exists(bookCacheDir.c_str())) {
      renderer.clearScreen();
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, renderer.getScreenHeight() / 2, "正在加载...章节多的书耗时较长", true,
                                EpdFontFamily::BOLD);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      auto& idx = M4IndexingState::Session::get().machine();
      M4ReaderOpenDispatch::onOpenBeginPreparing(idx, "open_book");
      Serial.printf("[%lu] %s\n", millis(), M4IndexingState::formatLogLine(idx.snap()).c_str());
    }
  }

  currentBookPath = initialBookPath;

  if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      indexTerminalFail("xtc_load");
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxtWithConversion(initialBookPath);
    if (!txt) {
      indexTerminalFail("txt_load");
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      indexTerminalFail("epub_load");
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

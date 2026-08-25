#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/providers/M4NovelProviderContract.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace M4PluginTocList {
class PagedTitleSource;
}

class NativeProviderBookActivity final : public ActivityWithSubactivity {
 public:
  NativeProviderBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             std::string providerId, std::string bookId,
                             std::string appId, std::string title, std::string author,
                             const std::function<void()>& onExitBook,
                             bool autoStartReading = false, int autoOpenIndex = -1,
                             std::string coverUrl = {});

  NativeProviderBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             std::string providerId, std::string bookId,
                             std::string appId, std::string title,
                             const std::function<void()>& onExitBook,
                             bool autoStartReading = false, int autoOpenIndex = -1,
                             std::string coverUrl = {})
      : NativeProviderBookActivity(renderer, mappedInput, providerId, bookId, appId,
                                   title, std::string(), onExitBook, autoStartReading,
                                   autoOpenIndex, std::move(coverUrl)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override;

  bool isFullscreenActivity() const override { return true; }
  int uiTextScalePercent() const override { return M4UiRuntimePolicy::kNativePluginTextScalePercent; }
  uint8_t touchFooterButtonsMask() const override {
    return M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm | M4FooterTouchPolicy::Left;
  }

 private:
  enum class State { Detail, CatalogLoading, Toc, Loading, Login, Reader, Error };
  enum class PendingCatalogAction { None, OpenToc, StartReading };

  bool prepareCatalog();
  bool startCatalogBootstrap(PendingCatalogAction action);
  void continueAfterCatalogReady();
  void loadBookDetail();
  void pollDetailLoading();
  void cancelDetailLoading();
  void renderDetail();
  void openToc();
  void startReading();
  void requestChapter(int index0, bool fromToc = false);
  void openLogin();
  bool openReadyReader(int index0);
  std::string titleAt(int index0) const;
  void renderCatalogLoading(bool force = false);
  void renderLoading(bool force = false);
  void renderError();

  std::string providerId_;
  std::string bookId_;
  std::string appId_;
  std::string title_;
  std::string author_;
  std::string coverUrl_;
  std::string appDataRoot_;
  std::string error_;
  std::function<void()> onExitBook_;
  std::shared_ptr<M4PluginTocList::PagedTitleSource> titles_;
  int chapterCount_ = 0;
  int currentIndex_ = 0;
  int loadingIndex_ = -1;
  std::string loadingTitle_;
  State state_ = State::Detail;
  PendingCatalogAction pendingCatalogAction_ = PendingCatalogAction::None;
  bool loadingFromToc_ = false;
  size_t pendingInitialByteOffset_ = 0;
  bool hasPendingInitialByteOffset_ = false;
  int pendingInitialIndex_ = -1;
  bool autoStartReading_ = false;
  int autoOpenIndex_ = -1;
  bool tocBackPending_ = false;
  bool tocSelectionPending_ = false;
  int tocSelectedIndex_ = -1;
  bool readerBackPending_ = false;
  bool loginFinishedPending_ = false;
  bool loginSucceeded_ = false;
  uint32_t lastCatalogPaintMs_ = 0;
  uint32_t lastLoadingPaintMs_ = 0;
  uint32_t catalogStartAtMs_ = 0;
  uint32_t chapterStartAtMs_ = 0;
  uint32_t chapterLoadStartedAtMs_ = 0;
  bool catalogStartPending_ = false;
  bool chapterStartPending_ = false;
  std::string lastCatalogSignature_;
  std::string lastLoadingSignature_;

  M4NovelProvider::BookDetail detail_;
  bool detailLoading_ = false;
  bool detailAttempted_ = false;
  std::string detailError_;
  std::string providerCoverBmpPath_;
  int detailReadButtonTop_ = 0;
  int detailReadButtonHeight_ = 0;
};

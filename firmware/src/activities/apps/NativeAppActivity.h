#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4xRegistry.h"
#include "apps/native/M4NativeUi.h"
#include "apps/native/M4NativeUiController.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class NativeAppActivity final : public ActivityWithSubactivity {
 public:
  NativeAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, M4xInstalledApp app,
                    const std::function<void()>& onExitApp);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override;

  // Native XML explicitly owns viewport/chrome policy at the <m4ui> root.
  // fullscreen=true keeps the app's own <buttons> footer and suppresses the
  // global Murphy `返回 | 主页` overlay. uiScale is a bounded draw-time scale.
  bool isFullscreenActivity() const override { return document_.fullscreen; }
  int uiTextScalePercent() const override { return document_.uiScalePercent; }

 private:
  bool loadDocument();
  const M4NativeUi::Screen* currentScreen() const;
  void render();
  void handleAction(const std::string& action, const M4NativeUi::Node* node = nullptr, int index0 = -1);
  bool rowAt(int index0, M4NativeUi::Row& out) const;
  std::string resolved(const std::string& s) const;
  void setError(const std::string& error);

  M4xInstalledApp app_;
  std::function<void()> onExitApp_;
  std::unique_ptr<M4NativeUi::Controller> controller_;
  M4NativeUi::Document document_;
  std::string screenId_;
  std::string error_;
  bool updateRequired_ = true;
  bool authLoginPrompted_ = false;
  uint32_t controllerRevision_ = 0;

  // One flex list per screen in v1. Other components remain fixed-height.
  int selectedIndex_ = 0;
  int tabIndex_ = 0;
  int listTop_ = 0;
  int listHeight_ = 0;
  int listCount_ = 0;
  std::string listSource_;
  std::string listNodeId_;
  std::string listAction_;

  // Bounded category tiles are a fixed 4-column touch target above the flex
  // list. They intentionally do not add another focus model: hardware keys
  // continue to operate the book list while M4 touch selects a category.
  int tilesTop_ = 0;
  int tilesHeight_ = 0;
  int tilesCount_ = 0;
  int tilesColumns_ = 4;
  const M4NativeUi::Node* tilesNode_ = nullptr;

  std::string buttonActions_[4];
};

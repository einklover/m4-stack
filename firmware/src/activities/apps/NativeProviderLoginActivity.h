#pragma once

#include "../Activity.h"

#include <functional>
#include <string>

class NativeProviderLoginActivity final : public Activity {
 public:
  NativeProviderLoginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              std::string providerId, std::string appDataRoot,
                              const std::function<void(bool)>& onFinished);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override;
  bool isFullscreenActivity() const override { return true; }
  uint8_t touchFooterButtonsMask() const override {
    return M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm;
  }

 private:
  void render(bool force = false);

  std::string providerId_;
  std::string appDataRoot_;
  std::function<void(bool)> onFinished_;
  std::string lastSignature_;
  uint32_t lastPaintMs_ = 0;
  bool delivered_ = false;
};
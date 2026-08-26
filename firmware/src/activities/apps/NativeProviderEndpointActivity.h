#pragma once

#include "../ActivityWithSubactivity.h"

#include "apps/providers/M4LegadoBridge.h"

#include <cstdint>
#include <functional>
#include <string>

class NativeProviderEndpointActivity final : public ActivityWithSubactivity {
 public:
  NativeProviderEndpointActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string providerId, std::string appId,
                                 const std::function<void(bool)>& onFinished);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override;

  bool isFullscreenActivity() const override { return true; }
  uint8_t touchFooterButtonsMask() const override;

 private:
  enum class Field : uint8_t { Host = 0, Port = 1 };

  void loadSavedEndpoint();
  void editField(Field field);
  void beginConnect();
  void finish(bool ok);
  void monitorConnection();
  void render(bool force = false);
  std::string connectionError(const std::string& code) const;
  std::string phaseKey() const;

  std::string providerId_;
  std::string appId_;
  std::string appDataRoot_;
  std::function<void(bool)> onFinished_;
  M4LegadoBridge::ManualEndpointState state_;
  std::string host_;
  std::string port_;
  Field selectedField_ = Field::Host;
  std::string lastSignature_;
  uint32_t lastPaintMs_ = 0;
  bool delivered_ = false;
};

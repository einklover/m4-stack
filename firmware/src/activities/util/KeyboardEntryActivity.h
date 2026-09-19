#pragma once
#include <GfxRenderer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "../Activity.h"
#include "network/KeyboardWebInputServer.h"
#include "util/TouchUiGeometry.h"

/**
 * Touch-first text entry activity for the 480x800 Murphy M4.
 *
 * The public constructor is intentionally stable so existing callers can adopt
 * the new keyboard without carrying legacy D-pad navigation code.
 */
class KeyboardEntryActivity : public Activity {
 public:
  using OnCompleteCallback = std::function<void(const std::string&)>;
  using OnCancelCallback = std::function<void()>;

  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "", const int startY = 10,
                                 const size_t maxLength = 0, const bool isPassword = false,
                                 OnCompleteCallback onComplete = nullptr, OnCancelCallback onCancel = nullptr)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        startY(startY),
        maxLength(maxLength),
        isPassword(isPassword),
        onComplete(std::move(onComplete)),
        onCancel(std::move(onCancel)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void show();
  void hide();

 private:
  std::string title;
  int startY;
  std::string text;
  size_t maxLength;
  bool isPassword;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  TouchHitGeometry::TouchKeyboardState keyboardState;
  bool passwordRevealed = false;
  int pressedKeyIndex = -1;
  bool showingQR = false;

  OnCompleteCallback onComplete;
  OnCancelCallback onCancel;
  std::unique_ptr<KeyboardWebInputServer> webInputServer;
  bool isVisible = true;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  TouchHitGeometry::TouchKeyboardLayout buildKeyboardLayout() const;
  TouchHitGeometry::Rect qrButtonRect() const;
  TouchHitGeometry::Rect visibilityButtonRect() const;
  void activateKey(const TouchHitGeometry::TouchKeyboardKey& key);
  void render() const;
  void renderQRScreen() const;
  void renderKeyBox(const TouchHitGeometry::Rect& rect, const char* label, bool isPressed) const;
  void startWebInputServer();
  void stopWebInputServer();
};
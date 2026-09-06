#include "KeyboardEntryActivity.h"

#include <Utf8.h>

#include "I18n.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkConstants.h"
#include "util/M4UiText.h"
#include "util/QRCodeHelper.h"
#include "util/TouchUiGeometry.h"

using TouchHitGeometry::TouchKeyboardKey;
using TouchHitGeometry::TouchKeyboardKeyKind;
using TouchHitGeometry::TouchKeyboardLayout;
using TouchHitGeometry::TouchKeyboardMode;
using TouchHitGeometry::TouchKeyboardState;

void KeyboardEntryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KeyboardEntryActivity*>(param);
  self->displayTaskLoop();
}

void KeyboardEntryActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;

  xTaskCreate(&KeyboardEntryActivity::taskTrampoline, "KeyboardEntryActivity",
              4096, this, 1, &displayTaskHandle);
}

void KeyboardEntryActivity::onExit() {
  Activity::onExit();
  stopWebInputServer();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

TouchKeyboardLayout KeyboardEntryActivity::buildKeyboardLayout() const {
  constexpr int keyHeight = 64;
  constexpr int keySpacing = 6;
  constexpr int bottomMargin = 12;

  const TouchKeyboardState& state = keyboardState;
  const int characterRows = state.mode == TouchKeyboardMode::Letters ? 3 : 4;
  const int totalRows = characterRows + 1;
  const int totalHeight = totalRows * keyHeight + (totalRows - 1) * keySpacing;
  const int keyboardStartY = renderer.getScreenHeight() - bottomMargin - totalHeight;

  return TouchHitGeometry::makeTouchKeyboardLayout(renderer.getScreenWidth(), keyboardStartY, keyHeight,
                                                    keySpacing, state.mode);
}

TouchHitGeometry::Rect KeyboardEntryActivity::qrButtonRect() const {
  return {12, 154, 142, 54};
}

TouchHitGeometry::Rect KeyboardEntryActivity::visibilityButtonRect() const {
  constexpr int width = 142;
  return {renderer.getScreenWidth() - 12 - width, 154, width, 54};
}

void KeyboardEntryActivity::activateKey(const TouchKeyboardKey& key) {
  switch (key.kind) {
    case TouchKeyboardKeyKind::Character:
      if (maxLength == 0 || text.length() < maxLength) {
        text += keyboardState.resolveCharacter(key.character);
      }
      break;
    case TouchKeyboardKeyKind::Mode:
      keyboardState.toggleMode();
      break;
    case TouchKeyboardKeyKind::Shift:
      keyboardState.toggleShift();
      break;
    case TouchKeyboardKeyKind::Space:
      if (maxLength == 0 || text.length() < maxLength) text += ' ';
      break;
    case TouchKeyboardKeyKind::Backspace:
      if (!text.empty()) utf8RemoveLastChar(text);
      break;
    case TouchKeyboardKeyKind::Confirm:
      if (onComplete) onComplete(text);
      break;
  }
  pressedKeyIndex = -1;
  updateRequired = true;
}

void KeyboardEntryActivity::loop() {
  if (!isVisible) return;

  if (mappedInput.wasBackGesture()) {
    if (showingQR) {
      stopWebInputServer();
      showingQR = false;
      updateRequired = true;
    } else if (onCancel) {
      onCancel();
    }
    return;
  }

  if (showingQR) {
    if (webInputServer && webInputServer->isRunning()) {
      webInputServer->handleClient();
      if (webInputServer->hasReceivedText()) {
        std::string received = webInputServer->consumeReceivedText();
        if (maxLength > 0) {
          if (text.length() >= maxLength) {
            received.clear();
          } else if (text.length() + received.length() > maxLength) {
            received.resize(maxLength - text.length());
          }
        }
        text += received;
        stopWebInputServer();
        showingQR = false;
        updateRequired = true;
      }
    }
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (qrButtonRect().contains(tx, ty)) {
      startWebInputServer();
      return;
    }
    if (isPassword && visibilityButtonRect().contains(tx, ty)) {
      passwordRevealed = !passwordRevealed;
      updateRequired = true;
      return;
    }

    const auto layout = buildKeyboardLayout();
    int hitIndex = -1;
    if (layout.hit(tx, ty, hitIndex)) {
      pressedKeyIndex = hitIndex;
      activateKey(layout.keys[hitIndex]);
    }
    return;
  }

  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const auto layout = buildKeyboardLayout();
    int hitIndex = -1;
    const int nextPressed = layout.hit(tx, ty, hitIndex) ? hitIndex : -1;
    if (pressedKeyIndex != nextPressed) {
      pressedKeyIndex = nextPressed;
      updateRequired = true;
    }
  }
}

void KeyboardEntryActivity::render() const {
  if (!isVisible) return;
  if (showingQR) {
    renderQRScreen();
    return;
  }

  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();

  M4UiText::drawCentered(renderer, UI_12_FONT_ID, startY, title.c_str(), true, EpdFontFamily::BOLD);

  constexpr int inputX = 12;
  constexpr int inputY = 54;
  constexpr int inputH = 82;
  renderer.drawRect(inputX, inputY, pageWidth - inputX * 2, inputH);

  std::string displayText;
  if (isPassword && !passwordRevealed) {
    displayText.assign(text.length(), '*');
  } else {
    displayText = text;
  }
  if (displayText.size() > 38) {
    displayText = "..." + displayText.substr(displayText.size() - 35);
  }
  displayText += "_";
  M4UiText::draw(renderer, UI_10_FONT_ID, inputX + 10, inputY + 27, displayText.c_str());

  const auto qrRect = qrButtonRect();
  renderKeyBox(qrRect, "扫码输入", false);
  if (isPassword) {
    const auto visibilityRect = visibilityButtonRect();
    renderKeyBox(visibilityRect, passwordRevealed ? "隐藏密码" : "显示密码", false);
  }

  const auto layout = buildKeyboardLayout();
  for (int i = 0; i < layout.keyCount; ++i) {
    const auto& key = layout.keys[i];
    std::string label;
    switch (key.kind) {
      case TouchKeyboardKeyKind::Character: {
        char c = key.character;
        if (keyboardState.mode == TouchKeyboardMode::Letters && keyboardState.shifted && c >= 'a' && c <= 'z') {
          c = static_cast<char>(c - 'a' + 'A');
        }
        label.assign(1, c);
        break;
      }
      case TouchKeyboardKeyKind::Mode:
        label = keyboardState.mode == TouchKeyboardMode::Letters ? "123" : "ABC";
        break;
      case TouchKeyboardKeyKind::Shift:
        label = "Shift";
        break;
      case TouchKeyboardKeyKind::Space:
        label = "空格";
        break;
      case TouchKeyboardKeyKind::Backspace:
        label = "退格";
        break;
      case TouchKeyboardKeyKind::Confirm:
        label = "确认";
        break;
    }
    const bool activeShift = key.kind == TouchKeyboardKeyKind::Shift && keyboardState.shifted;
    renderKeyBox(key.rect, label.c_str(), i == pressedKeyIndex || activeShift);
  }

  renderer.displayBuffer();
}

void KeyboardEntryActivity::renderKeyBox(const TouchHitGeometry::Rect& rect, const char* label,
                                         const bool isPressed) const {
  if (isPressed) {
    renderer.fillRectDither(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2, LightGray);
  }
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const int textWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, label);
  const int textX = rect.x + (rect.width - textWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  M4UiText::draw(renderer, UI_10_FONT_ID, textX, textY, label);
}

void KeyboardEntryActivity::show() {
  isVisible = true;
  updateRequired = true;
}

void KeyboardEntryActivity::hide() {
  isVisible = false;
  updateRequired = true;
}

void KeyboardEntryActivity::renderQRScreen() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int LINE_SPACING = 28;
  constexpr int QR_TOTAL = QRCodeHelper::qrSize();

  renderer.clearScreen();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kEnterText), true, EpdFontFamily::BOLD);

  if (webInputServer && webInputServer->isRunning()) {
    if (webInputServer->isApMode()) {
      int y = 55;
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, "Hotspot Mode", true, EpdFontFamily::BOLD);
      const std::string ssidInfo = "Network: " + webInputServer->getApSSID();
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y + LINE_SPACING, ssidInfo.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 2, L(Str::kConnectWifi));
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 3, L(Str::kOrScanQRCode));

      const std::string wifiQR = webInputServer->getWifiQRString();
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, y + LINE_SPACING * 4, wifiQR);
      y += QR_TOTAL - 4 * QRCodeHelper::DEFAULT_PX + 3 * LINE_SPACING;

      const std::string url = webInputServer->getUrl();
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y + LINE_SPACING * 3, url.c_str(), true, EpdFontFamily::BOLD);
      const std::string ipUrl = "or http://" + webInputServer->getIP() + "/";
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 4, ipUrl.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 5, L(Str::kOpenInBrowser));
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 6, L(Str::kOrScanQR));
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, y + LINE_SPACING * 7, url);
    } else {
      constexpr int y = 65;
      const std::string ip = webInputServer->getIP();
      const std::string ipInfo = "IP Address: " + ip;
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, ipInfo.c_str());
      const std::string webUrl = "http://" + ip + "/";
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y + LINE_SPACING * 2, webUrl.c_str(), true, EpdFontFamily::BOLD);
      const std::string hostnameUrl = std::string("or http://") + NetworkConstants::AP_HOSTNAME + ".local/";
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 3, hostnameUrl.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 4, L(Str::kOpenInBrowser));
      renderer.drawCenteredText(SMALL_FONT_ID, y + LINE_SPACING * 5, L(Str::kOrScanQR));
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, y + LINE_SPACING * 6, webUrl);
    }
  } else {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 20, "Starting server...", true,
                           EpdFontFamily::BOLD);
  }

  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动返回");
  renderer.displayBuffer();
}

void KeyboardEntryActivity::startWebInputServer() {
  if (!webInputServer) webInputServer.reset(new KeyboardWebInputServer());
  if (!webInputServer->isRunning()) webInputServer->start();
  showingQR = true;
  pressedKeyIndex = -1;
  updateRequired = true;
}

void KeyboardEntryActivity::stopWebInputServer() {
  if (webInputServer) {
    webInputServer->stop();
    webInputServer.reset();
  }
}
#include "KeyboardEntryActivity.h"

#include "MappedInputManager.h"
#include "I18n.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "network/NetworkConstants.h"
#include "util/QRCodeHelper.h"
#include "util/TouchHitGeometry.h"
#include <Utf8.h>

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "Q<O",  // Row 0: QR, Backspace(<-), OK - rendered specially
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'@#", "zxcvbnm,./$%&",
    "^  ____"  // ^ = shift, _ = space (bottom row)
};

// Keyboard layouts - uppercase/symbols
const char* const KeyboardEntryActivity::keyboardShift[NUM_ROWS] = {"~!@#$%^&*()_+", "QWERTYUIOP{}|", "ASDFGHJKL:\"",
                                                                    "ZXCVBNM<>?", "SPECIAL ROW"};

// Shift state strings
const char* const KeyboardEntryActivity::shiftString[3] = {"shift", "SHIFT", "LOCK"};

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

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&KeyboardEntryActivity::taskTrampoline, "KeyboardEntryActivity",
              4096,               // Stack size (increased for QR code rendering)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void KeyboardEntryActivity::onExit() {
  Activity::onExit();

  // Stop web input server if running
  stopWebInputServer();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

int KeyboardEntryActivity::getRowLength(const int row) const {
  if (row < 0 || row >= NUM_ROWS) return 0;

  // Return actual length of each row based on keyboard layout
  switch (row) {
    case 0:
      return 5;   // QR, OK, BS, shift, space
    case 1:
      return 13;  // `1234567890-=
    case 2:
      return 13;  // qwertyuiop[]backslash
    case 3:
      return 13;  // asdfghjkl;'@#
    case 4:
      return 13;  // zxcvbnm,./$%&
    case 5:
      return 0;   // Row 5 removed (shift+space moved to row 0)
    default:
      return 0;
  }
}

char KeyboardEntryActivity::getSelectedChar() const {
  const char* const* layout = shiftState ? keyboardShift : keyboard;

  if (selectedRow < 0 || selectedRow >= NUM_ROWS) return '\0';
  if (selectedCol < 0 || selectedCol >= getRowLength(selectedRow)) return '\0';

  return layout[selectedRow][selectedCol];
}

void KeyboardEntryActivity::handleKeyPress() {
  // Row 0 control buttons: QR, OK, Backspace, shift, space
  if (selectedRow == 0) {
    if (selectedCol == ROW0_QR_COL) {
      // QR button - start web input server and show QR screen
      startWebInputServer();
      return;
    }
    if (selectedCol == ROW0_OK_COL) {
      // Done button
      if (onComplete) {
        onComplete(text);
      }
      return;
    }
    if (selectedCol == ROW0_BACKSPACE_COL) {
      // Backspace (UTF-8 aware to handle Chinese and other multi-byte chars)
      if (!text.empty()) {
        utf8RemoveLastChar(text);
      }
      return;
    }
    if (selectedCol == ROW0_SHIFT_COL) {
      // Shift toggle (0 = lower case, 1 = upper case, 2 = shift lock)
      shiftState = (shiftState + 1) % 3;
      return;
    }
    if (selectedCol == ROW0_SPACE_COL) {
      // Space bar
      if (maxLength == 0 || text.length() < maxLength) {
        text += ' ';
      }
      return;
    }
  }

  // Regular character
  const char c = getSelectedChar();
  if (c == '\0') {
    return;
  }

  if (maxLength == 0 || text.length() < maxLength) {
    text += c;
    // Auto-disable shift after typing a character in non-lock mode
    if (shiftState == 1) {
      shiftState = 0;
    }
  }
}

KeyboardEntryActivity::KeyMetrics KeyboardEntryActivity::computeKeyMetrics(int pageWidth, int pageHeight,
                                                                           int inputEndY) const {
  KeyMetrics m;
  m.touchMode = mappedInput.hasTouch();
  if (m.touchMode) {
    // Finger-friendly: taller keys, pin to bottom, reclaim hint chrome space.
    m.keyHeight = 48;
    m.keySpacing = 4;
    m.bottomReserve = 14;  // no physical button-hint strip
    m.btnPadding = 14;
  } else {
    m.keyHeight = 28;
    m.keySpacing = 5;
    m.bottomReserve = 40;  // physical button hints
    m.btnPadding = 12;
  }

  // Full-width keys (13 columns) with small side margin on touch.
  const int sideMargin = m.touchMode ? 6 : 0;
  const int usableW = pageWidth - sideMargin * 2;
  m.keyWidth = (usableW / KEYS_PER_ROW) - m.keySpacing;
  if (m.keyWidth < 20) m.keyWidth = 20;
  m.maxRowWidth = KEYS_PER_ROW * (m.keyWidth + m.keySpacing);
  m.leftMargin = (pageWidth - m.maxRowWidth) / 2;

  const int keyboardH = VISIBLE_KEY_ROWS * (m.keyHeight + m.keySpacing) - m.keySpacing;
  const int availableTop = inputEndY + (m.touchMode ? 8 : 10);
  const int availableBottom = pageHeight - m.bottomReserve;
  if (m.touchMode) {
    // Pin keyboard to bottom for thumb reach; leave title/input above.
    m.keyboardStartY = availableBottom - keyboardH;
    if (m.keyboardStartY < availableTop) m.keyboardStartY = availableTop;
  } else {
    m.keyboardStartY = availableTop + (availableBottom - availableTop - keyboardH) / 2;
  }
  return m;
}

TouchHitGeometry::KeyboardLayout KeyboardEntryActivity::buildHitLayout(const KeyMetrics& m) const {
  const int pageWidth = renderer.getScreenWidth();
  const int row0W[5] = {
      M4UiText::textWidth(renderer, UI_10_FONT_ID, "\xe6\x89\xab\xe7\xa0\x81\xe8\xbe\x93\xe5\x85\xa5") + m.btnPadding * 2,
      M4UiText::textWidth(renderer, UI_10_FONT_ID, "\xe7\xa1\xae\xe8\xae\xa4") + m.btnPadding * 2,
      M4UiText::textWidth(renderer, UI_10_FONT_ID, "\xe9\x80\x80\xe6\xa0\xbc") + m.btnPadding * 2,
      M4UiText::textWidth(renderer, UI_10_FONT_ID, shiftString[shiftState]) + m.btnPadding * 2,
      48,
  };
  const int charLens[4] = {getRowLength(1), getRowLength(2), getRowLength(3), getRowLength(4)};
  return TouchHitGeometry::makeKeyboardLayout(pageWidth, m.keyboardStartY, m.keyHeight, m.keySpacing, KEYS_PER_ROW,
                                              row0W, charLens);
}

void KeyboardEntryActivity::loop() {
  // If not visible (hidden by parent) ignore all input except maybe QR exit
  if (!isVisible) return;

  // Touch: edge-back + full key-grid hit (geometry shared with render())
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      if (showingQR) {
        stopWebInputServer();
        showingQR = false;
        selectedRow = 0;
        selectedCol = ROW0_QR_COL;
        updateRequired = true;
        return;
      }
      if (onCancel) onCancel();
      return;
    }
    if (!showingQR) {
      // Mirror render(): wrap password/plain text to get the same inputEndY / keyboard Y.
      const int pageWidth = renderer.getScreenWidth();
      const int pageHeight = renderer.getScreenHeight();
      std::string displayText = isPassword ? std::string(text.length(), '*') : text;
      displayText += "_";
      int inputEndY = startY + 22;
      int lineStartIdx = 0;
      int lineEndIdx = static_cast<int>(displayText.length());
      while (true) {
        std::string lineText = displayText.substr(static_cast<size_t>(lineStartIdx),
                                                  static_cast<size_t>(lineEndIdx - lineStartIdx));
        const int textWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, lineText.c_str());
        if (textWidth <= pageWidth - 40) {
          if (lineEndIdx == static_cast<int>(displayText.length())) break;
          inputEndY += renderer.getLineHeight(UI_10_FONT_ID);
          lineStartIdx = lineEndIdx;
          lineEndIdx = static_cast<int>(displayText.length());
        } else {
          lineEndIdx -= 1;
        }
      }
      inputEndY += renderer.getLineHeight(SMALL_FONT_ID) + 2;  // touch tip line
      const auto metrics = computeKeyMetrics(pageWidth, pageHeight, inputEndY);
      const auto layout = buildHitLayout(metrics);

      int tx = 0, ty = 0;
      // Prefer tap to activate key once; down only highlights
      if (mappedInput.wasScreenTapped(tx, ty)) {
        int row = -1, col = -1;
        if (layout.hit(tx, ty, row, col)) {
          selectedRow = row;
          selectedCol = col;
          handleKeyPress();
          updateRequired = true;
        }
        return;
      }
      if (mappedInput.wasScreenTouchDown(tx, ty)) {
        int row = -1, col = -1;
        if (layout.hit(tx, ty, row, col)) {
          if (selectedRow != row || selectedCol != col) {
            selectedRow = row;
            selectedCol = col;
            updateRequired = true;
          }
        }
        return;
      }
    }
  }

  // In QR mode, only handle Back button and web server polling
  if (showingQR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      stopWebInputServer();
      showingQR = false;
      selectedRow = 0;
      selectedCol = ROW0_QR_COL;
      updateRequired = true;
    }

    // Poll the web server for incoming requests
    if (webInputServer && webInputServer->isRunning()) {
      webInputServer->handleClient();

      if (webInputServer->hasReceivedText()) {
        std::string received = webInputServer->consumeReceivedText();
        if (maxLength > 0 && text.length() + received.length() > maxLength) {
          received.resize(maxLength - text.length());
        }
        text += received;
        // Return to keyboard view with focus on the confirm button
        stopWebInputServer();
        showingQR = false;
        selectedRow = 0;
        selectedCol = ROW0_OK_COL;
        updateRequired = true;
      }
    }
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (selectedRow > 0) {
      selectedRow--;
    } else {
      // Wrap to row 4 (last character row)
      selectedRow = 4;
    }
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol > maxCol) selectedCol = maxCol;
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (selectedRow < 4) {
      selectedRow++;
    } else {
      // Wrap to top row (row 0 with control buttons)
      selectedRow = 0;
    }
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol > maxCol) selectedCol = maxCol;
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Row 0 has special widths
    if (selectedRow == 0) {
      if (selectedCol == ROW0_QR_COL) {
        selectedCol = ROW0_SPACE_COL;
      } else {
        selectedCol--;
      }
      updateRequired = true;
      return;
    }

    // Normal rows
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol > 0) {
      selectedCol--;
    } else {
      selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Row 0 has special widths
    if (selectedRow == 0) {
      if (selectedCol == ROW0_SPACE_COL) {
        selectedCol = ROW0_QR_COL;
      } else {
        selectedCol++;
      }
      updateRequired = true;
      return;
    }

    // Normal rows
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol < maxCol) {
      selectedCol++;
    } else {
      selectedCol = 0;
    }
    updateRequired = true;
  }

  // Selection - 使用 wasReleased 避免冒泡到父 Activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleKeyPress();
    updateRequired = true;
  }

  // Cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (onCancel) {
      onCancel();
    }
    updateRequired = true;
  }
}

void KeyboardEntryActivity::render() const {
  // do nothing when hidden; parent should redraw its own contents
  if (!isVisible) return;

  if (showingQR) {
    renderQRScreen();
    return;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool touchMode = mappedInput.hasTouch();

  renderer.clearScreen();

  // --- Draw title at top ---
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY, title.c_str());

  // --- Draw input field below title ---
  const int inputStartY = startY + 22;
  int inputEndY = inputStartY;
  M4UiText::draw(renderer, UI_10_FONT_ID, 10, inputStartY, "[");

  std::string displayText;
  if (isPassword) {
    displayText = std::string(text.length(), '*');
  } else {
    displayText = text;
  }

  // Show cursor at end
  displayText += "_";

  // Render input text across multiple lines
  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    const int textWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, lineText.c_str());
    if (textWidth <= pageWidth - 40) {
      M4UiText::draw(renderer, UI_10_FONT_ID, 20, inputEndY, lineText.c_str());
      if (lineEndIdx == displayText.length()) {
        break;
      }

      inputEndY += renderer.getLineHeight(UI_10_FONT_ID);
      lineStartIdx = lineEndIdx;
      lineEndIdx = displayText.length();
    } else {
      lineEndIdx -= 1;
    }
  }
  M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 15, inputEndY, "]");

  // Touch: compact gesture tip (no physical-button chrome)
  if (touchMode) {
    inputEndY += renderer.getLineHeight(SMALL_FONT_ID) + 2;
    renderer.drawCenteredText(SMALL_FONT_ID, inputEndY, "点按输入 · 左缘滑动返回");
  }

  const KeyMetrics m = computeKeyMetrics(pageWidth, pageHeight, inputEndY);
  const int keyHeight = m.keyHeight;
  const int keySpacing = m.keySpacing;
  const int keyWidth = m.keyWidth;
  const int leftMargin = m.leftMargin;
  const int maxRowWidth = m.maxRowWidth;
  const int keyboardStartY = m.keyboardStartY;
  const int btnPadding = m.btnPadding;

  const char* const* layout = shiftState ? keyboardShift : keyboard;

  // Row 0: [扫码输入] [确认] [退格] [shift] [空格]  control buttons, left-aligned
  {
    const int rowY = keyboardStartY;
    int currentX = leftMargin;

    // "扫码输入" button
    const int qrTextW = M4UiText::textWidth(renderer, UI_10_FONT_ID, "\xe6\x89\xab\xe7\xa0\x81\xe8\xbe\x93\xe5\x85\xa5");
    const int qrBtnW = qrTextW + btnPadding * 2;
    const bool qrSelected = (selectedRow == 0 && selectedCol == ROW0_QR_COL);
    renderKeyBox(currentX, rowY, qrBtnW, keyHeight, "\xe6\x89\xab\xe7\xa0\x81\xe8\xbe\x93\xe5\x85\xa5", qrSelected);
    currentX += qrBtnW + keySpacing;

    // "确认" button
    const int okTextW = M4UiText::textWidth(renderer, UI_10_FONT_ID, "\xe7\xa1\xae\xe8\xae\xa4");
    const int okBtnW = okTextW + btnPadding * 2;
    const bool okSelected = (selectedRow == 0 && selectedCol == ROW0_OK_COL);
    renderKeyBox(currentX, rowY, okBtnW, keyHeight, "\xe7\xa1\xae\xe8\xae\xa4", okSelected);
    currentX += okBtnW + keySpacing;

    // "退格" button
    const int bsTextW = M4UiText::textWidth(renderer, UI_10_FONT_ID, "\xe9\x80\x80\xe6\xa0\xbc");
    const int bsBtnW = bsTextW + btnPadding * 2;
    const bool bsSelected = (selectedRow == 0 && selectedCol == ROW0_BACKSPACE_COL);
    renderKeyBox(currentX, rowY, bsBtnW, keyHeight, "\xe9\x80\x80\xe6\xa0\xbc", bsSelected);
    currentX += bsBtnW + keySpacing;

    // SHIFT key
    const int shiftTextW = M4UiText::textWidth(renderer, UI_10_FONT_ID, shiftString[shiftState]);
    const int shiftBtnW = shiftTextW + btnPadding * 2;
    const bool shiftSelected = (selectedRow == 0 && selectedCol == ROW0_SHIFT_COL);
    renderKeyBox(currentX, rowY, shiftBtnW, keyHeight, shiftString[shiftState], shiftSelected);
    currentX += shiftBtnW + keySpacing;

    // Space bar (spans remaining width)
    const int spaceW = leftMargin + maxRowWidth - currentX;
    const bool spaceSelected = (selectedRow == 0 && selectedCol == ROW0_SPACE_COL);
    renderKeyBox(currentX, rowY, spaceW, keyHeight, "\xe7\xa9\xba\xe6\xa0\xbc", spaceSelected);
  }

  // Rows 1-4: regular character keys (each in a box)
  for (int row = 1; row < NUM_ROWS - 1; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);

    for (int col = 0; col < getRowLength(row); col++) {
      const char c = layout[row][col];
      std::string keyLabel(1, c);

      const int boxX = leftMargin + col * (keyWidth + keySpacing);
      const bool isSelected = row == selectedRow && col == selectedCol;
      renderKeyBox(boxX, rowY, keyWidth, keyHeight, keyLabel.c_str(), isSelected);
    }
  }

  // Physical-key devices only: show Back/Select/D-pad chrome.
  // Touch M4 has no front buttons — do not draw misleading button hints.
  if (!touchMode) {
    const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kSelect), L(Str::kLeft), L(Str::kRight));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    GUI.drawSideButtonHints(renderer, L(Str::kUp), L(Str::kDown), true);
  }

  renderer.displayBuffer();
}



void KeyboardEntryActivity::renderKeyBox(const int boxX, const int boxY, const int boxW, const int boxH,
                                         const char* label, const bool isSelected) const {
  if (isSelected) {
    // Fill with gray background when selected
    renderer.fillRectDither(boxX + 1, boxY + 1, boxW - 2, boxH - 2, LightGray);
  } else if (mappedInput.hasTouch()) {
    // Light fill so larger touch targets read as tappable keys on e-ink
    renderer.drawRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2);
  }

  // Draw box border
  renderer.drawRect(boxX, boxY, boxW, boxH);

  // Center text within box
  const int textWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, label);
  const int textX = boxX + (boxW - textWidth) / 2;
  const int textY = boxY + (boxH - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  M4UiText::draw(renderer, UI_10_FONT_ID, textX, textY, label);
}

// visibility helpers
void KeyboardEntryActivity::show() {
    isVisible = true;
    updateRequired = true;
}

void KeyboardEntryActivity::hide() {
    isVisible = false;
    updateRequired = true;
}

void KeyboardEntryActivity::renderQRScreen() const {
  const auto pageWidth = renderer.getScreenWidth();

  // Use same line spacing as File Transfer for consistency
  constexpr int LINE_SPACING = 28;
  // QR size: same as File Transfer (6px per module)
  constexpr int QR_TOTAL = QRCodeHelper::qrSize();  // 198px

  renderer.clearScreen();

  // Title - matching File Transfer style
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kEnterText), true, EpdFontFamily::BOLD);

  if (webInputServer && webInputServer->isRunning()) {
    if (webInputServer->isApMode()) {
      // === AP mode layout (matching File Transfer) ===
      int apStartY = 55;

      M4UiText::drawCentered(renderer, UI_10_FONT_ID, apStartY, "Hotspot Mode", true, EpdFontFamily::BOLD);

      std::string ssidInfo = "Network: " + webInputServer->getApSSID();
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, apStartY + LINE_SPACING, ssidInfo.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 2, L(Str::kConnectWifi));

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 3,
                                L(Str::kOrScanQRCode));

      // WiFi QR code (same size as File Transfer)
      const std::string wifiQR = webInputServer->getWifiQRString();
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, apStartY + LINE_SPACING * 4, wifiQR);

      apStartY += QR_TOTAL - 4 * QRCodeHelper::DEFAULT_PX + 3 * LINE_SPACING;

      // URL section
      const std::string url = webInputServer->getUrl();
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, apStartY + LINE_SPACING * 3, url.c_str(), true, EpdFontFamily::BOLD);

      // Show IP address as fallback
      std::string ipUrl = "or http://" + webInputServer->getIP() + "/";
      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 4, ipUrl.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 5, L(Str::kOpenInBrowser));

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 6, L(Str::kOrScanQR));

      // URL QR code (same size as File Transfer)
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, apStartY + LINE_SPACING * 7, url);

    } else {
      // === STA mode layout (WiFi already connected, matching File Transfer) ===
      constexpr int staStartY = 65;

      const std::string ip = webInputServer->getIP();

      std::string ipInfo = "IP Address: " + ip;
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, staStartY, ipInfo.c_str());

      // Show web server URL prominently
      std::string webUrl = "http://" + ip + "/";
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, staStartY + LINE_SPACING * 2, webUrl.c_str(), true, EpdFontFamily::BOLD);

      // Also show hostname URL using shared constant
      std::string hostnameUrl = std::string("or http://") + NetworkConstants::AP_HOSTNAME + ".local/";
      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 3, hostnameUrl.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 4, L(Str::kOpenInBrowser));

      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 5, L(Str::kOrScanQR));

      // URL QR code (same size as File Transfer)
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, staStartY + LINE_SPACING * 6, webUrl);
    }
  } else {
    const auto pageHeight = renderer.getScreenHeight();
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 20, "Starting server...", true, EpdFontFamily::BOLD);
  }

  // Physical keys only — touch uses edge-back / on-screen controls
  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  } else {
    const auto pageHeight = renderer.getScreenHeight();
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动返回");
  }

  renderer.displayBuffer();
}

void KeyboardEntryActivity::startWebInputServer() {
  if (!webInputServer) {
    webInputServer.reset(new KeyboardWebInputServer());
  }

  if (!webInputServer->isRunning()) {
    webInputServer->start();
  }

  showingQR = true;
  updateRequired = true;
}

void KeyboardEntryActivity::stopWebInputServer() {
  if (webInputServer) {
    webInputServer->stop();
    webInputServer.reset();
  }
}
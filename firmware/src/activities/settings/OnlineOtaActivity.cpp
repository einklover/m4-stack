#include "OnlineOtaActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

void OnlineOtaActivity::taskTrampoline(void* param) {
  static_cast<OnlineOtaActivity*>(param)->displayTaskLoop();
}

void OnlineOtaActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  xTaskCreate(&OnlineOtaActivity::taskTrampoline, "OnlineOtaTask",
              4096, this, 1, &displayTaskHandle);

  // 先检查WiFi状态，决定下一步
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[%lu] [OnlineOTA] WiFi already connected, checking version...\n", millis());
    state = CHECKING_VERSION;
    updateRequired = true;
    startVersionCheck();
  } else {
    Serial.printf("[%lu] [OnlineOTA] WiFi not connected, launching WiFi selection...\n", millis());
    WiFi.mode(WIFI_STA);
    // 直接启动WiFi选择，不显示中间状态
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) {
                                                 exitActivity();
                                                 if (connected) {
                                                   Serial.printf("[%lu] [OnlineOTA] WiFi connected via selection\n", millis());
                                                   xSemaphoreTake(renderingMutex, portMAX_DELAY);
                                                   state = CHECKING_VERSION;
                                                   xSemaphoreGive(renderingMutex);
                                                   updateRequired = true;
                                                   startVersionCheck();
                                                 } else {
                                                   Serial.printf("[%lu] [OnlineOTA] WiFi selection cancelled\n", millis());
                                                   goBack();
                                                 }
                                               }));
  }
}

void OnlineOtaActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void OnlineOtaActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void OnlineOtaActivity::render() {
  if (subActivity) {
    // 子Activity存在时，只渲染子Activity，不渲染父级内容
    return;
  }

  renderer.clearScreen();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kSystemUpgrade), true, EpdFontFamily::BOLD);
  
  switch (state) {
    case CHECKING_VERSION:
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, L(Str::kCheckingUpdate), true, EpdFontFamily::BOLD);
      break;
  
    case NO_UPDATE: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, L(Str::kAlreadyLatest), true, EpdFontFamily::BOLD);
      std::string versionStr = std::string(L(Str::kVersionNumber)) + std::to_string(otaManager.getUpdateInfo().localVersion);
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, versionStr.c_str());
      const auto labels = mappedInput.mapLabels(L(Str::kBackShort), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case UPDATE_AVAILABLE: {
      const auto& info = otaManager.getUpdateInfo();
      // 先显示版本信息（固定位置，始终可见）
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + 4;
      int versionY = 95;

      std::string serverVer = std::string(L(Str::kLatestVersion)) + std::to_string(info.remoteVersion);
      M4UiText::draw(renderer, UI_10_FONT_ID, 20, versionY, serverVer.c_str());
      versionY += lineHeight;
                
      std::string localVer = std::string(L(Str::kCurrentVersion)) + std::to_string(info.localVersion);
      M4UiText::draw(renderer, UI_10_FONT_ID, 20, versionY, localVer.c_str());
      versionY += lineHeight + 10;  // 版本号和备注之间留10px间距
        
      // Render remark text (multi-line support with word wrap)
      int remarkY = versionY;  // 版本号下方开始显示备注
      const std::string& remark = info.remark;
      const int pageWidth = renderer.getScreenWidth();
      const int textAreaWidth = pageWidth - 40;  // 左右各刉20px边距
      
      // 自动换行：将长文本分割成多行
      std::vector<std::string> wrappedLines;
      size_t pos = 0;
      while (pos < remark.size()) {
        size_t nl = remark.find('\n', pos);
        std::string line;
        if (nl == std::string::npos) {
          line = remark.substr(pos);
          pos = remark.size();
        } else {
          line = remark.substr(pos, nl - pos);
          pos = nl + 1;
        }
        
        // 移除行尾的 \r（Windows 换行符）
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        
        // 如果行太长，需要拆分。
        // 修复策略：使用 getTextWidth 精确测量像素宽度，逐个 UTF-8 字符推进，
        // 确保断点始终在字符边界（避免中文被截成乱码），且不超出屏幕宽度。
        while (!line.empty()) {
          // 整行能放下则直接存入
          if (M4UiText::textWidth(renderer, UI_10_FONT_ID, line.c_str()) <= textAreaWidth) {
            wrappedLines.push_back(line);
            break;
          }

          // 逐字符推进，找到最长能放下的前缀（lastGoodByte 始终在 UTF-8 字符边界）
          size_t curByte = 0;
          size_t lastGoodByte = 0;
          while (curByte < line.size()) {
            const uint8_t b = static_cast<uint8_t>(line[curByte]);
            size_t charLen;
            if      (b < 0x80)           charLen = 1;
            else if ((b & 0xE0) == 0xC0) charLen = 2;
            else if ((b & 0xF0) == 0xE0) charLen = 3;
            else                         charLen = 4;
            const size_t nextByte = std::min(curByte + charLen, line.size());
            // 测量到 nextByte 为止的前缀宽度
            const std::string prefix = line.substr(0, nextByte);
            if (M4UiText::textWidth(renderer, UI_10_FONT_ID, prefix.c_str()) > textAreaWidth) {
              break;  // 超宽，停在上一个字符边界
            }
            lastGoodByte = nextByte;
            curByte = nextByte;
          }
          // 极端情况：连一个字符都放不下，强制放入一个字符防止死循环
          if (lastGoodByte == 0) {
            const uint8_t b = static_cast<uint8_t>(line[0]);
            lastGoodByte = (b < 0x80) ? 1 : ((b & 0xF0) == 0xE0) ? 3 : ((b & 0xE0) == 0xC0) ? 2 : 4;
            if (lastGoodByte > line.size()) lastGoodByte = line.size();
          }

          // 向前查找合适断点（空格或 ASCII 标点），最多回看 10 个字符
          size_t breakPos = lastGoodByte;
          size_t scanBack = lastGoodByte;
          for (int lookBack = 0; lookBack < 10 && scanBack > 0; lookBack++) {
            size_t prev = scanBack - 1;
            while (prev > 0 && (static_cast<uint8_t>(line[prev]) & 0xC0) == 0x80) prev--;
            const char c = line[prev];
            if (c == ' ' || c == ',' || c == '.') {
              breakPos = prev + 1;
              break;
            }
            scanBack = prev;
          }

          wrappedLines.push_back(line.substr(0, breakPos));
          line = line.substr(breakPos);
        }
      }
      
      // 计算最大可见行数：充分利用屏幕空间
      const int pageHeight = renderer.getScreenHeight();
      const int buttonHintsHeight = 40;  // 按钮提示区域高度
      const int maxVisibleArea = pageHeight - versionY - buttonHintsHeight - 10;  // 从版本号下方到按钮提示上方的所有空间
      remarkMaxLines = maxVisibleArea / lineHeight;
      
      // 确保滚动偏移不越界
      int totalLines = wrappedLines.size();
      if (remarkScrollOffset > std::max(0, totalLines - remarkMaxLines)) {
        remarkScrollOffset = std::max(0, totalLines - remarkMaxLines);
      }
      
      // 绘制可见的文本行
      int linesDrawn = 0;
      for (size_t i = remarkScrollOffset; i < wrappedLines.size() && linesDrawn < remarkMaxLines; i++, linesDrawn++) {
        M4UiText::draw(renderer, UI_10_FONT_ID, 20, remarkY, wrappedLines[i].c_str());
        remarkY += lineHeight;
      }
      
      // 绘制滚动条
      if (totalLines > remarkMaxLines) {
        const int scrollbarX = pageWidth - 10;
        const int scrollbarY = versionY;  // 与备注起始Y坐标一致
        const int scrollbarHeight = remarkMaxLines * lineHeight;
        const int thumbHeight = std::max(20, scrollbarHeight * remarkMaxLines / totalLines);
        const int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * remarkScrollOffset / (totalLines - remarkMaxLines);
        
        // 滚动条轨道
        renderer.drawRect(scrollbarX, scrollbarY, 6, scrollbarHeight);
        // 滚动条滑块
        renderer.fillRect(scrollbarX + 1, thumbY, 4, thumbHeight);
      }

      const auto labels = mappedInput.mapLabels(L(Str::kCancel), L(Str::kConfirm), "\u2191", "\u2193");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);  // 强制显示按钮提示
      break;
    }

    case CHECK_FAILED: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, L(Str::kCheckFailed), true, EpdFontFamily::BOLD);
      if (!errorMessage.empty()) {
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, errorMessage.c_str());
      }
      const auto labels = mappedInput.mapLabels(L(Str::kBackShort), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case DOWNLOADING: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, L(Str::kDownloadingFirmware), true, EpdFontFamily::BOLD);

      const auto pageWidth = renderer.getScreenWidth();
      const int barX = 20;
      const int barY = 310;
      const int barWidth = pageWidth - 40;
      const int barHeight = 50;

      float pct = (totalBytes > 0)
                      ? static_cast<float>(downloadedBytes) / static_cast<float>(totalBytes)
                      : 0.0f;
      if (pct > 1.0f) pct = 1.0f;
      unsigned int pctInt = static_cast<unsigned int>(pct * 100);

      renderer.drawRect(barX, barY, barWidth, barHeight);
      int fillWidth = static_cast<int>(pct * static_cast<float>(barWidth - 8));
      if (fillWidth > 0) {
        renderer.fillRect(barX + 4, barY + 4, fillWidth, barHeight - 8);
      }

      // Show "X.X MB / Y.Y MB  ZZ%"
      char progressText[64];
      float dlMB = static_cast<float>(downloadedBytes) / (1024.0f * 1024.0f);
      float totalMB = static_cast<float>(totalBytes) / (1024.0f * 1024.0f);
      snprintf(progressText, sizeof(progressText), "%.1f MB / %.1f MB  %u%%", dlMB, totalMB, pctInt);
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, barY + barHeight + 15, progressText);
      break;
    }

    case DOWNLOAD_FAILED: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, L(Str::kDownloadFailed), true, EpdFontFamily::BOLD);
      if (!errorMessage.empty()) {
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, errorMessage.c_str());
      }
      const auto labels2 = mappedInput.mapLabels(L(Str::kBackShort), "", "", "");
      GUI.drawButtonHints(renderer, labels2.btn1, labels2.btn2, labels2.btn3, labels2.btn4);
      break;
    }

    case VERIFYING_MD5:
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, L(Str::kVerifyingFirmware), true, EpdFontFamily::BOLD);
      break;

    case MD5_FAILED: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, L(Str::kVerifyFailed), true, EpdFontFamily::BOLD);
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, L(Str::kFirmwareIncomplete));
      const auto labels3 = mappedInput.mapLabels(L(Str::kBackShort), "", "", "");
      GUI.drawButtonHints(renderer, labels3.btn1, labels3.btn2, labels3.btn3, labels3.btn4);
      break;
    }

    case READY_TO_FLASH: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 120, L(Str::kFirmwareVerifyPass), true, EpdFontFamily::BOLD);
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 160, L(Str::kUpgradeTakesTime));
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 190, L(Str::kUpgradeNoResponse));
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 220, L(Str::kDoNotOperate));
      
      // 醒目的提示框：粗边框 + 黑底白字效果
      const auto pageWidth = renderer.getScreenWidth();
      const int promptY = 275;
      const int promptHeight = 55;
      const int promptX = 15;
      const int promptWidth = pageWidth - 30;
      
      // 绘制粗黑边框（通过绘制多个矩形实现）
      for (int i = 0; i < 4; i++) {
        renderer.drawRect(promptX - i, promptY - i, promptWidth + i * 2, promptHeight + i * 2);
      }
      
      // 填充黑色背景
      renderer.fillRect(promptX, promptY, promptWidth, promptHeight);
      
      // 在黑色背景上绘制白色文字（black=false）
      // 由于e-ink特性，white=不绘制，所以文字区域会保持黑色背景的空白
      // 我们通过绘制周围的黑色来"刻出"白色文字的效果
      // 简单方案：直接绘制，系统会处理反色
      M4UiText::drawCentered(renderer, UI_12_FONT_ID, promptY + 18, L(Str::kPressConfirmToFlash), false, EpdFontFamily::BOLD);
      
      const auto labels = mappedInput.mapLabels(L(Str::kCancel), L(Str::kConfirm), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case FLASHING: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, L(Str::kUpgradingFirmware), true, EpdFontFamily::BOLD);

      const auto pageWidth = renderer.getScreenWidth();
      const int barX = 20;
      const int barY = 310;
      const int barWidth = pageWidth - 40;
      const int barHeight = 50;

      float pct = (flashTotal > 0)
                      ? static_cast<float>(flashDone) / static_cast<float>(flashTotal)
                      : 0.0f;
      if (pct > 1.0f) pct = 1.0f;
      unsigned int pctInt = static_cast<unsigned int>(pct * 100);

      renderer.drawRect(barX, barY, barWidth, barHeight);
      int fillWidth = static_cast<int>(pct * static_cast<float>(barWidth - 8));
      if (fillWidth > 0) {
        renderer.fillRect(barX + 4, barY + 4, fillWidth, barHeight - 8);
      }

      M4UiText::drawCentered(renderer, UI_10_FONT_ID, barY + barHeight + 15,
                             (std::to_string(pctInt) + "%").c_str());
      break;
    }

    case FLASH_FAILED: {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, L(Str::kInstallFailed), true, EpdFontFamily::BOLD);
      if (!errorMessage.empty()) {
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, errorMessage.c_str());
      }
      const auto labels4 = mappedInput.mapLabels(L(Str::kBackShort), "", "", "");
      GUI.drawButtonHints(renderer, labels4.btn1, labels4.btn2, labels4.btn3, labels4.btn4);
      break;
    }

    case FINISHED:
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, L(Str::kInstallingFirmware), true, EpdFontFamily::BOLD);
      break;

    case SHUTTING_DOWN:
      // Nothing to render during shutdown
      break;
  }

  renderer.displayBuffer();
}

void OnlineOtaActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    return;
  }

  // CHECKING_WIFI 状态已经在 onEnter 中处理，不再需要

  if (state == NO_UPDATE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == UPDATE_AVAILABLE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [OnlineOTA] User confirmed download\n", millis());
      startDownload();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    // 支持滚动备注文本（上下左右键都可以）
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      if (remarkScrollOffset > 0) {
        remarkScrollOffset--;
        updateRequired = true;
      }
      xSemaphoreGive(renderingMutex);
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      remarkScrollOffset++;  // 会在render中自动校正越界
      updateRequired = true;
      xSemaphoreGive(renderingMutex);
    }
    return;
  }

  if (state == CHECK_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == DOWNLOAD_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == MD5_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == READY_TO_FLASH) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [OnlineOTA] User confirmed flash\n", millis());
      
      // 检查可用内存
      size_t freeHeap = xPortGetFreeHeapSize();
      Serial.printf("[%lu] [OnlineOTA] Free heap before flash: %zu bytes\n", millis(), freeHeap);
      
      if (freeHeap < 50 * 1024) {  // 低于50KB
        Serial.printf("[%lu] [OnlineOTA] Insufficient memory, showing warning\n", millis());
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        errorMessage = std::string(L(Str::kInsufficientMemory)) + std::to_string(freeHeap / 1024) + "KB)";
        state = FLASH_FAILED;  // 复用失败状态显示
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }
      
      startFlash();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == FLASH_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}

void OnlineOtaActivity::startVersionCheck() {
  auto result = otaManager.checkForUpdate();

  if (result == OtaManager::OK) {
    Serial.printf("[%lu] [OnlineOTA] Update available\n", millis());
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = UPDATE_AVAILABLE;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
  } else if (result == OtaManager::NO_UPDATE) {
    Serial.printf("[%lu] [OnlineOTA] Already up to date\n", millis());
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = NO_UPDATE;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
  } else {
    Serial.printf("[%lu] [OnlineOTA] Check failed: %d\n", millis(), result);
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = CHECK_FAILED;
    switch (result) {
      case OtaManager::HTTP_ERROR:
        errorMessage = L(Str::kNetworkRequestFailed);
        break;
      case OtaManager::PARSE_ERROR:
        errorMessage = L(Str::kVersionParseFailed);
        break;
      case OtaManager::WIFI_NOT_CONNECTED:
        errorMessage = L(Str::kWifiDisconnected);
        break;
      default:
        errorMessage = L(Str::kUnknownError);
        break;
    }
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
  }
}

void OnlineOtaActivity::startDownload() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = DOWNLOADING;
  downloadedBytes = 0;
  totalBytes = 0;
  lastPercentage = UNINITIALIZED_PERCENTAGE;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;

  xTaskCreate(
      [](void* param) {
        auto* self = static_cast<OnlineOtaActivity*>(param);

        Serial.printf("[%lu] [OnlineOTA] Starting firmware download...\n", millis());

        auto result = self->otaManager.downloadFirmware(
            [self](size_t downloaded, size_t total) {
              self->downloadedBytes = downloaded;
              self->totalBytes = total;
              self->updateRequired = true;
            });

        if (result != OtaManager::OK) {
          Serial.printf("[%lu] [OnlineOTA] Download failed: %d\n", millis(), result);
          self->otaManager.cleanupDownloadedFiles();
          xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
          self->state = DOWNLOAD_FAILED;
          switch (result) {
            case OtaManager::HTTP_ERROR:
              self->errorMessage = L(Str::kNetworkRequestFailed);
              break;
            case OtaManager::FILE_ERROR:
              self->errorMessage = L(Str::kFileWriteFailed);
              break;
            case OtaManager::WIFI_NOT_CONNECTED:
              self->errorMessage = L(Str::kWifiDisconnected);
              break;
            default:
              self->errorMessage = L(Str::kUnknownError);
              break;
          }
          xSemaphoreGive(self->renderingMutex);
          self->updateRequired = true;
          vTaskDelete(NULL);
          return;
        }

        Serial.printf("[%lu] [OnlineOTA] Download complete, verifying MD5...\n", millis());
        xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
        self->state = VERIFYING_MD5;
        xSemaphoreGive(self->renderingMutex);
        self->updateRequired = true;

        auto md5Result = self->otaManager.verifyMd5();
        if (md5Result == OtaManager::OK) {
          Serial.printf("[%lu] [OnlineOTA] MD5 verification passed\n", millis());
          xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
          self->state = READY_TO_FLASH;
          xSemaphoreGive(self->renderingMutex);
          self->updateRequired = true;
        } else {
          Serial.printf("[%lu] [OnlineOTA] MD5 verification failed\n", millis());
          xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
          self->state = MD5_FAILED;
          xSemaphoreGive(self->renderingMutex);
          self->updateRequired = true;
        }

        vTaskDelete(NULL);
      },
      "OtaDownloadTask", 12288, this, 1, nullptr);
}

void OnlineOtaActivity::startFlash() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = FLASHING;
  flashDone = 0;
  flashTotal = 0;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;

  xTaskCreate(
      [](void* param) {
        auto* self = static_cast<OnlineOtaActivity*>(param);

        Serial.printf("[%lu] [OnlineOTA] Starting firmware flash...\n", millis());

        auto result = self->otaManager.flashFirmware(
            [self](size_t done, size_t total) {
              self->flashDone = done;
              self->flashTotal = total;
              self->updateRequired = true;
            });

        if (result != OtaManager::OK) {
          Serial.printf("[%lu] [OnlineOTA] Flash failed: %d\n", millis(), result);
          xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
          self->state = FLASH_FAILED;
          self->errorMessage = L(Str::kFlashingError);
          xSemaphoreGive(self->renderingMutex);
          self->updateRequired = true;
          vTaskDelete(NULL);
          return;
        }

        Serial.printf("[%lu] [OnlineOTA] Flash complete, restarting...\n", millis());
        xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
        self->state = FINISHED;
        xSemaphoreGive(self->renderingMutex);
        self->updateRequired = true;

        // Give render task time to display the "升级完成" message
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        ESP.restart();
      },
      "OtaFlashTask", 12288, this, 1, nullptr);
}

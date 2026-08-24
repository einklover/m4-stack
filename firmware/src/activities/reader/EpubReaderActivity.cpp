#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "I18n.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "BookmarkManagerActivity.h"
#include "BookmarkStore.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"

#ifdef CROSSPOINT_X3
#include "TiltPageTurnDetector.h"
extern TiltPageTurnDetector tiltDetector;
#endif
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/StringUtils.h"
#include "util/TouchHitGeometry.h"

#include "JianGuoSyncActivity.h"
#include "EpubReaderSettingsActivity.h"
#include "EpdFontLoader.h"
#include "activities/settings/SimpleBluetoothActivity.h"

#include <sys/time.h>
#include <ctime>
#include <Utf8.h>

#ifdef CROSSPOINT_X3
#include "DS3231RTC.h"
#else
// NTP同步函数声明（定义在main.cpp中）
extern void syncNtpTime();
#endif


bool EpubReaderActivity::isLandscapeDualPage() const {
  return SETTINGS.landscapeDualPageEnabled &&
         (renderer.getOrientation() == GfxRenderer::LandscapeClockwise ||
          renderer.getOrientation() == GfxRenderer::LandscapeCounterClockwise);
}


namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 0;
constexpr int progressBarBottomGap = 5;  // 进度条距屏幕底部间距（有状态栏文字时）
constexpr int progressBarBottomGapOnly = 8;  // 仅进度条模式的底部间距
constexpr int progressBarTextGap = 1;    // 文字底部距进度条顶部间距
constexpr int statusBarTopGap = 2;       // 状态栏顶部距阅读区域底部间距
// pages per minute for each auto-turn option index (index 0 = off/unused, 1-4 = speeds)
const int PAGE_TURN_LABELS[] = {1, 1, 3, 6, 12};
constexpr int PAGE_TURN_LABELS_SIZE = 5;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// ── Clock display helpers ───────────────────────────────────────────────────
constexpr time_t VALID_TIME_THRESHOLD = 1704067200;  // 2024-01-01 00:00:00 UTC
constexpr int UTC8_OFFSET = 8 * 3600;
constexpr int clockIconRadius = 9;
constexpr int clockIconSize = clockIconRadius * 2;
constexpr int clockBatterySpacing = 8;
constexpr int clockIconTextSpacing = 4;

std::string getClockTimeString() {
  time_t utcTime = 0;
#ifdef CROSSPOINT_X3
  utcTime = DS3231RTC::readTime();
  if (utcTime < VALID_TIME_THRESHOLD) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    utcTime = tv.tv_sec;
  }
#else
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  utcTime = tv.tv_sec;
#endif
  if (utcTime < VALID_TIME_THRESHOLD) {
    return "--:--";
  }
  // 设置时区后 localtime_r 会自动处理时区转换，不再需要手动 +UTC8_OFFSET
  struct tm tmInfo;
  localtime_r(&utcTime, &tmInfo);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", tmInfo.tm_hour, tmInfo.tm_min);
  return std::string(buf);
}

void drawClockIcon(const GfxRenderer& renderer, int cx, int cy, int radius) {
  // 双层 Bresenham 圆（加粗表盘轮廓）
  for (int r = radius; r >= radius - 1; r--) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
      renderer.drawPixel(cx + x, cy + y);
      renderer.drawPixel(cx + y, cy + x);
      renderer.drawPixel(cx - y, cy + x);
      renderer.drawPixel(cx - x, cy + y);
      renderer.drawPixel(cx - x, cy - y);
      renderer.drawPixel(cx - y, cy - x);
      renderer.drawPixel(cx + y, cy - x);
      renderer.drawPixel(cx + x, cy - y);
      y++;
      if (err < 0) {
        err += 2 * y + 1;
      } else {
        x--;
        err += 2 * (y - x) + 1;
      }
    }
  }
  // 用 fillRect 画指针（墨水屏上填充矩形最清晰）
  // 指针整体右下偏移1px，对齐圆的视觉中心
  int pcx = cx + 1, pcy = cy + 1;
  // 时针：9点方向（向左），从圆心略右开始，主体向左
  int hLen = radius * 2 / 3;
  renderer.fillRect(pcx - hLen, pcy - 1, hLen + 2, 3, true);
  // 分针：12点方向（向上），从圆心略下开始，主体向上
  int mLen = radius - 2;
  renderer.fillRect(pcx - 1, pcy - mLen, 3, mLen + 2, true);
  // 中心点 3x3
  renderer.fillRect(pcx - 1, pcy - 1, 3, 3, true);
}

// Apply the logical reader orientation to the renderer.
// This centralizes orientation mapping so we don't duplicate switch logic elsewhere.
void applyReaderOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

}  // namespace

EpubReaderActivity::EPUBState EpubReaderActivity::state;

void EpubReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  state = EPUBState::READING;

  // 每次进入阅读器时重置自动翻页开关，需用户手动开启
  // 注：rollingMode/pageTurnDuration保留用户设置，开启时从SETTINGS读取
  automaticPageTurnActive = false;
  rollingHalfTurned = false;
  if (SETTINGS.autoPageTurnEnabled) {
    SETTINGS.autoPageTurnEnabled = 0;
    SETTINGS.saveToFile();
  }


  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  applyReaderOrientation(renderer, SETTINGS.orientation);

  renderingMutex = xSemaphoreCreateMutex();

  epub->setupCacheDir();


  FsFile f;
  if (SdMan.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
            // Validation: If loaded index is invalid, reset to 0
      if (currentSpineIndex >= epub->getSpineItemsCount()) {
        Serial.printf("[%lu] [ERS] Loaded invalid spine index %d (max %d), resetting\n", millis(), currentSpineIndex,
                      epub->getSpineItemsCount());
        currentSpineIndex = 0;
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      Serial.printf("[%lu] [ERS] Opened for first time, navigating to text reference at index %d\n", millis(),
                    textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  // 传递 originalSourcePath，以便从最近阅读打开时能知道原始 TXT 文件路径
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath(), originalSourcePath);

  // Check if there's a pending bookmark jump from the home screen bookmark notes
  if (APP_STATE.pendingBookmarkPercent >= 0.0f) {
    jumpToPercent(APP_STATE.pendingBookmarkPercent);
    APP_STATE.pendingBookmarkPercent = -1.0f;
  }

  // Trigger first update
  updateRequired = true;

#ifndef CROSSPOINT_X3
  // ========== 进入阅读器时同步NTP时间（X4平台） ==========
  // 每次开机只同步一次，由 APP_STATE.ntpSyncedThisBoot 控制
  // 在主线程中同步，避免WiFi驱动FreeRTOS队列问题
  syncNtpTime();
#endif

  // ========== 蓝牙初始化 ==========
  // 若蓝牙已启用（例如上次长按确认键连接过），保持现有连接不做处理。
  // 若尚未启用，不在打开书籍时自动连接，由用户通过长按确认键按需触发。
  {
    auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) {
      Serial.printf("[%lu] [BT] 蓝牙已启用，保持现有状态\n", millis());
    } else {
      Serial.printf("[%lu] [BT] 蓝牙未启用，跳过自动连接（用户可长按确认键手动连接）\n", millis());
    }
  }

  // 开始阅读统计会话
  READING_STATS.startSession();

  xTaskCreate(&EpubReaderActivity::taskTrampoline, "EpubReaderActivityTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // 结束阅读统计会话并保存
  READING_STATS.endSession();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  if(state== EPUBState::READING){
    // 自动翻页逻辑：在所有其他处理之前检查，以便优先处理取消操作
    if (automaticPageTurnActive && !subActivity) {
      // 按下 Confirm 或 Back 键取消自动翻页
      if (!skipNextButtonCheck &&
          (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
           mappedInput.wasReleased(MappedInputManager::Button::Back))) {
        automaticPageTurnActive = false;
        rollingMode = false;
        rollingHalfTurned = false;
        if (SETTINGS.autoPageTurnEnabled) {
          SETTINGS.autoPageTurnEnabled = 0;
          SETTINGS.saveToFile();
        }
        updateRequired = true;  // 刷新状态栏以移除自动翻页指示
        return;
      }

      // 在自动翻页激活期间也需要清除 skipNextButtonCheck
      // 否则通过长按开启自动翻页后，该标志将永久无法被清除，导致无法取消
      if (skipNextButtonCheck) {
        const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                    !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
        // BT虚拟按键会持续inject Back事件，wasReleased(Back)始终为true，不能作为clearing条件
        // 只检查物理按键状态（isPressed是物理专用），确保BT注入不会卡住skipNextButtonCheck
        const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back);
        if (confirmCleared && backCleared) {
          skipNextButtonCheck = false;
        }
        return;
      }

      if (!section) {
        updateRequired = true;
        return;
      }

      // 若渲染正在进行，跳过本轮（等渲染完成后再计时）
      if (!APP_STATE.isRenderComplete) {
        return;
      }

      // 到达翻页时间间隔，执行翻页
      if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
        // 横屏双页模式：绕过卷帘机制，左右交替接收新页
        if (isLandscapeDualPage() && rollingMode) {
          rollingHalfTurned = false;
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          if (dualNextLeft) {
            // 左侧更新：新左页 = 当前右页 + 1
            int newLeft = dualRightPage + 1;
            if (dualRightPage >= 0 && newLeft < section->pageCount) {
              section->currentPage = newLeft;
              dualNextLeft = false;
            } else {
              // 已到 section 末，进入下一 spine
              nextPageNumber = 0;
              currentSpineIndex++;
              section.reset();
              dualRightPage = -1;
              dualNextLeft = true;
            }
          } else {
            // 右侧更新：新右页 = 当前左页 + 1
            int newRight = section->currentPage + 1;
            if (newRight < section->pageCount) {
              dualRightPage = newRight;
              dualNextLeft = true;
            } else {
              // 左侧已是 section 最后一页，进入下一 spine
              nextPageNumber = 0;
              currentSpineIndex++;
              section.reset();
              dualRightPage = -1;
              dualNextLeft = true;
            }
          }
          xSemaphoreGive(renderingMutex);
          lastPageTurnTime = millis();
          updateRequired = true;
          return;
        }

        // 卷帘模式第一阶段：触发半屏翻转（显示下一页顶部 + 当前页底部）
        if (rollingMode && !rollingHalfTurned) {
          rollingHalfTurned = true;
          lastPageTurnTime = millis();
          updateRequired = true;
          return;
        }

        // 完整翻页（普通模式，或卷帘模式第二阶段）
        rollingHalfTurned = false;
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (section->currentPage < section->pageCount - 1) {
          section->currentPage++;
        } else {
          nextPageNumber = 0;
          currentSpineIndex++;
          section.reset();
        }
        xSemaphoreGive(renderingMutex);
        lastPageTurnTime = millis();
        updateRequired = true;
        return;
      }
      return;  // 等待计时器到期
    }

    // 从设置中读取全局下一页模式状态
    globalNextPageMode = (SETTINGS.globalNextPageModeEnabled != 0);
    
    // 长按菜单键 (Confirm) 执行映射功能 (1 秒以上)
    if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 1000) {
      // 检查是否已经处理过这次长按（避免重复触发）
      if (!globalNextPageModeToggled) {
        globalNextPageModeToggled = true;
        skipNextButtonCheck = true;  // 吸收紧随其后的按键松手事件
        const uint8_t action = SETTINGS.longPressConfirmAction;
        if (action == 0) {
          // 切换全局下一页模式
          globalNextPageMode = !globalNextPageMode;
          SETTINGS.globalNextPageModeEnabled = globalNextPageMode ? 1 : 0;
          SETTINGS.saveToFile();
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          GUI.drawPopup(renderer, globalNextPageMode ? L(Str::kGlobalNextPageModeOn) : L(Str::kGlobalNextPageModeOff));
          renderer.displayBuffer();
          xSemaphoreGive(renderingMutex);
        } else if (action == 1) {
          // 长按蓝牙：已连接则提示，未连接则触发连接（不再切换开关）
          try {
            auto& btMgr = BluetoothHIDManager::getInstance();
            if (btMgr.isEnabled() && !btMgr.getConnectedDevices().empty()) {
              // 已有蓝牙设备连接中，仅提示用户
              xSemaphoreTake(renderingMutex, portMAX_DELAY);
              GUI.drawPopup(renderer, L(Str::kBTDeviceConnected));
              renderer.displayBuffer();
              xSemaphoreGive(renderingMutex);
            } else {
              // 未连接：尝试启用并连接上次配对设备
              GUI.drawPopup(renderer, L(Str::kBTConnectingEllipsis));
              renderer.displayBuffer(HalDisplay::FAST_REFRESH);
              bool connected = false;
              bool enabled = btMgr.isEnabled();
              if (!enabled) {
                // 蓝牙未启用，先启用
                SETTINGS.bluetoothEnabled = 1;
                SETTINGS.saveToFile();
                enabled = btMgr.enable();
                if (!enabled) {
                  SETTINGS.bluetoothEnabled = 0;
                  SETTINGS.saveToFile();
                }
              }
              if (enabled) {
                btMgr.startScan(2000);
                std::string lastAddr, lastName;
                if (btMgr.loadLastConnectedDevice(lastAddr, lastName) && !lastAddr.empty()) {
                  unsigned long btStart = millis();
                  while (millis() - btStart < 3000) {
                    if (btMgr.connectToDevice(lastAddr)) {
                      connected = true;
                      SETTINGS.bluetoothEnabled = 1;
                      SETTINGS.saveToFile();
                      break;
                    }
                    delay(200);
                  }
                }
                btMgr.stopScan();
                if (!connected) {
                  btMgr.disable();
                  SETTINGS.bluetoothEnabled = 0;
                  SETTINGS.saveToFile();
                }
              }
              xSemaphoreTake(renderingMutex, portMAX_DELAY);
              GUI.drawPopup(renderer, connected ? L(Str::kBTConnected) : L(Str::kBTConnectFailed));
              renderer.displayBuffer();
              xSemaphoreGive(renderingMutex);
            }
          } catch (...) {
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            GUI.drawPopup(renderer, L(Str::kBTError));
            renderer.displayBuffer();
            xSemaphoreGive(renderingMutex);
          }
        } else if (action == 2) {
          // 切换自动翻页
          SETTINGS.autoPageTurnEnabled = SETTINGS.autoPageTurnEnabled ? 0 : 1;
          SETTINGS.saveToFile();
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          GUI.drawPopup(renderer, SETTINGS.autoPageTurnEnabled ? L(Str::kAutoPageTurnOn) : L(Str::kAutoPageTurnOff));
          renderer.displayBuffer();
          xSemaphoreGive(renderingMutex);
          if (SETTINGS.autoPageTurnEnabled) applyAutoPageTurnSettings();
        } else if (action == 3) {
          // 切换抗锯齿
          SETTINGS.textAntiAliasing = SETTINGS.textAntiAliasing ? 0 : 1;
          SETTINGS.saveToFile();
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          GUI.drawPopup(renderer, SETTINGS.textAntiAliasing ? L(Str::kAntiAliasingOn) : L(Str::kAntiAliasingOff));
          renderer.displayBuffer();
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
        } else if (action == 4) {
          // 切换暗黑模式
          SETTINGS.epubDarkMode = SETTINGS.epubDarkMode ? 0 : 1;
          SETTINGS.saveToFile();
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          GUI.drawPopup(renderer, SETTINGS.epubDarkMode ? L(Str::kDarkModeOn) : L(Str::kDarkModeOff));
          renderer.displayBuffer();
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
#ifdef CROSSPOINT_X3
        } else if (action == 5) {
          // 切换晃动翻页
          SETTINGS.tiltPageTurnEnabled = SETTINGS.tiltPageTurnEnabled ? 0 : 1;
          SETTINGS.saveToFile();
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          GUI.drawPopup(renderer, SETTINGS.tiltPageTurnEnabled ? L(Str::kTiltPageTurnOn) : L(Str::kTiltPageTurnOff));
          renderer.displayBuffer();
          xSemaphoreGive(renderingMutex);
#endif
        }
        // action == maxAction（"无"）时不执行任何动作
      }
      return;
    }
    
    // 重置长按标志，当按键释放后允许下次触发
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      globalNextPageModeToggled = false;
    }
    
    // 长按上一页键 (PageBack) 切换横屏/竖屏模式 (1 秒以上)
    if (mappedInput.isPressed(MappedInputManager::Button::PageBack) && mappedInput.getHeldTime() >= 1000) {
      // 检查是否已经处理过这次长按（避免重复触发）
      if (!longPressPageBackToggled) {
        longPressPageBackToggled = true;
        skipNextButtonCheck = true;  // 吸收紧随其后的按键松手事件

        // 根据当前方向切换：横屏 (LANDSCAPE_CW) <-> 竖屏 (PORTRAIT)
        uint8_t targetOrientation;
        const char* popupMessage;

        if (SETTINGS.orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW) {
          // 当前是横屏，切换回竖屏（按钮在下侧）
          targetOrientation = CrossPointSettings::ORIENTATION::PORTRAIT;
          popupMessage = L(Str::kSwitchToPortrait);
        } else {
          // 其他情况切换到横屏（按钮在右侧）
          targetOrientation = CrossPointSettings::ORIENTATION::LANDSCAPE_CCW;
          popupMessage = L(Str::kSwitchToLandscape);
        }

        Serial.printf("[%lu] [ERS] Long press PageBack detected, switching orientation to %d\n", millis(), targetOrientation);

        // 显示弹框提示
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        GUI.drawPopup(renderer, popupMessage);
        renderer.displayBuffer();
        xSemaphoreGive(renderingMutex);

        // 应用方向设置
        applyOrientation(targetOrientation);
        updateRequired = true;
      }
      return;
    }

    // 重置长按上一页键标志
    if (!mappedInput.isPressed(MappedInputManager::Button::PageBack)) {
      longPressPageBackToggled = false;
    }

    // 在全局下一页模式下，短按菜单键不进入菜单（阻止冒泡），转为翻页
    // if (globalNextPageMode && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    //   // 阻止进入菜单，转为翻页
    //   if (section && section->currentPage < section->pageCount - 1) {
    //     section->currentPage++;
    //   } else {
    //     xSemaphoreTake(renderingMutex, portMAX_DELAY);
    //     nextPageNumber = 0;
    //     currentSpineIndex++;
    //     section.reset();
    //     xSemaphoreGive(renderingMutex);
    //   }
    //   updateRequired = true;
    //   return;
    // }
    // Pass input responsibility to sub activity if exists.
    // pumpSubActivityFrame defers menu->settings replacement until loop returns,
    // so opening 阅读设置 no longer destroys EpubReaderMenu mid-stack.
    if (subActivity) {
      const bool replaced = pumpSubActivityFrame();
      if (replaced) {
        skipNextButtonCheck = true;
        // 子页面退出时 onGoBack 已置 updateRequired=true 请求重绘；仅当替换出
        // 新子页面时才清除，避免吞掉合法的阅读器重绘请求。
        if (subActivity) {
          updateRequired = false;
        } else {
          updateRequired = true;
        }
      }
      // Deferred exit: process after subActivity->loop() returns to avoid use-after-free
      if (pendingSubactivityExit) {
        pendingSubactivityExit = false;
        exitActivity();
        updateRequired = true;
        skipNextButtonCheck = true;  // Skip button processing to ignore stale events
      }
      // Deferred go home: process after subActivity->loop() returns to avoid race condition
      if (pendingGoHome) {
        pendingGoHome = false;
        exitActivity();
        if (onGoHome) {
          onGoHome();
        }
        return;  // Don't access 'this' after callback
      }
      return;
    }

    // Handle pending go back when no subactivity (e.g., from short press back)
    if (pendingGoBack) {
      pendingGoBack = false;
      if (onGoBack) {
        onGoBack();
      }
      return;  // Don't access 'this' after callback
    }

    // Handle pending go home when no subactivity (e.g., from long press back)
    if (pendingGoHome) {
      pendingGoHome = false;
      if (onGoHome) {
        onGoHome();
      }
      return;  // Don't access 'this' after callback
    }

    // Handle pending Bluetooth settings open (from long-press confirm)
    if (pendingBluetoothSettings) {
      pendingBluetoothSettings = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SimpleBluetoothActivity(
          renderer, mappedInput,
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    // Skip button processing after returning from subactivity
    // This prevents stale button release events from triggering actions
    // We wait until: (1) all relevant buttons are released, AND (2) wasReleased events have been cleared
    if (skipNextButtonCheck) {
      const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                  !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
      // BT虚拟按键会持续inject Back/PageBack事件，wasReleased始终为true，不能作为clearing条件
      // 只检查物理按键状态（isPressed是物理专用），确保BT注入不会卡住skipNextButtonCheck
      const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back);
      const bool pageBackCleared = !mappedInput.isPressed(MappedInputManager::Button::PageBack);
      if (confirmCleared && backCleared && pageBackCleared) {
        skipNextButtonCheck = false;
      }
      return;
    }

    // Touch: left/right thirds page turn; center or top-edge swipe opens menu.
    bool touchPrev = false;
    bool touchNext = false;
    bool touchMenu = false;
    if (mappedInput.hasTouch() && !subActivity) {
      if (mappedInput.wasBackGesture()) {
        pendingGoBack = true;
        return;
      }
      if (mappedInput.wasMenuGesture()) {
        touchMenu = true;
      } else {
        int tx = 0;
        int ty = 0;
        if (mappedInput.wasScreenTapped(tx, ty)) {
          const auto zone = TouchHitGeometry::readerZoneFromPoint(tx, ty, renderer.getScreenWidth(),
                                                                  renderer.getScreenHeight());
          if (zone == TouchHitGeometry::ReaderZone::Prev) {
            touchPrev = true;
          } else if (zone == TouchHitGeometry::ReaderZone::Next) {
            touchNext = true;
          } else if (zone == TouchHitGeometry::ReaderZone::Menu) {
            touchMenu = true;
          }
        } else {
          const auto swipe = mappedInput.wasSwipe();
          if (swipe == MappedInputManager::SwipeDir::Right) {
            touchPrev = true;
          } else if (swipe == MappedInputManager::SwipeDir::Left) {
            touchNext = true;
          }
        }
      }
    }

    // Enter reader menu activity (仅在全局下一页模式关闭时有效)
    if (!globalNextPageMode && (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || touchMenu)) {
      // Don't start activity transition while rendering
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterReaderMenu(EpubReaderMenuActivity::MenuLayer::QUICK);
      xSemaphoreGive(renderingMutex);
    }

    // Long press BACK (1s+) goes directly to home
    if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
      pendingGoHome = true;
      return;
    }

    // Short press BACK goes to file selection
    // Note: getHeldTime() is NOT checked here because it reflects physical button duration and
    // would block virtual (BT) button releases if the user previously held a physical key >= goHomeMs.
    // The long-press case (go home) is already handled above via isPressed(Back) && getHeldTime() >= goHomeMs,
    // so any wasReleased(Back) that reaches this point is always a short press or virtual button.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      pendingGoBack = true;
      return;
    }

    // 在全局下一页模式下：长按 PageBack 触发上一页，短按其他键（除菜单键外）都是下一页
    if (globalNextPageMode) {
      // 菜单键（Confirm）短按：进入菜单（不在全局下一页模式下拦截）
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        // Don't start activity transition while rendering
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        exitActivity();
        enterReaderMenu(EpubReaderMenuActivity::MenuLayer::QUICK);
        xSemaphoreGive(renderingMutex);
        return;
      }

      const bool longPressPageBack = mappedInput.wasReleased(MappedInputManager::Button::Power);
      
      if (longPressPageBack) {
        // 阻止事件冒泡，执行上一页
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (section && section->currentPage > 0) {
          section->currentPage--;
        } else {
          nextPageNumber = UINT16_MAX;
          currentSpineIndex--;
          section.reset();
        }
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }

      // 检查是否有任何按键被释放（用于短按翻页）- 排除菜单键（Confirm）
      const bool anyButtonReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                                     mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                                     mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                                     mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                                     mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                     mappedInput.wasReleased(MappedInputManager::Button::PageForward);

      if (anyButtonReleased) {
        // 短按：执行下一页
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (section && section->currentPage < section->pageCount - 1) {
          section->currentPage++;
        } else {
          nextPageNumber = 0;
          currentSpineIndex++;
          section.reset();
        }
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }

      // 跳过正常翻页逻辑
      return;
    }

    // When long-press chapter skip is disabled, turn pages on press instead of release.
    const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;

#ifdef CROSSPOINT_X3
    // 自动旋转检查：如果检测到方向变化，应用新方向
    if (SETTINGS.autoRotateEnabled && tiltDetector.isReady()) {
      uint8_t detected = tiltDetector.getDetectedOrientation();
      if (detected != SETTINGS.orientation) {
        Serial.printf("[%lu] [ERS] Auto-rotate applying: %d -> %d\n", millis(), SETTINGS.orientation, detected);
        applyOrientation(detected);
        updateRequired = true;
        return;
      }
    }
#endif

    // ==================== 翻页按键状态检测 ====================

    // isPhysicalButton 必须在 wasReleased 计算之前检测：
    //   - isPressed 在按住期间为 true，松手后立即变 false
    //   - 若在 wasReleased（松手事件）处理后再查 isPressed，必然为 false，导致物理长按条件永远不成立
    const bool isPhysicalButton = mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
                                  mappedInput.isPressed(MappedInputManager::Button::PageForward) ||
                                  mappedInput.isPressed(MappedInputManager::Button::Left) ||
                                  mappedInput.isPressed(MappedInputManager::Button::Right);

    // ====== 物理按键长按跳章（必须在 prevTriggered/nextTriggered 早期返回之前执行）======
    // 原理：判断物理长按必须在按住期间（isPressed=true）检测，不能依赖松手事件
    // physicalSkipFired：同一次长按内只触发一次跳章
    // physicalSkipAbsorb：吸收跳章后紧随而来的松手翻页事件，防止额外翻一页
    static bool physicalSkipFired  = false;
    static bool physicalSkipAbsorb = false;

    if (SETTINGS.longPressChapterSkip && isPhysicalButton && mappedInput.getHeldTime() > skipChapterMs) {
      if (!physicalSkipFired) {
        physicalSkipFired  = true;
        physicalSkipAbsorb = true;
        const bool skipPrev = mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
                              mappedInput.isPressed(MappedInputManager::Button::Left);
        Serial.printf("[%lu] [ERS] Physical chapter skip: dir=%s heldMs=%lu\n",
                      millis(), skipPrev ? "prev" : "next", mappedInput.getHeldTime());
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        nextPageNumber = 0;
        currentSpineIndex = skipPrev ? currentSpineIndex - 1 : currentSpineIndex + 1;
        section.reset();
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      }
      return;  // 按住期间持续返回，防止翻页
    }
    if (!isPhysicalButton) physicalSkipFired = false;

    const bool prevTriggered =
        touchPrev ||
        (usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                mappedInput.wasPressed(MappedInputManager::Button::Left))
                             : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                mappedInput.wasReleased(MappedInputManager::Button::Left)));
    const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                              mappedInput.wasReleased(MappedInputManager::Button::Power);
    
    // Legacy power-button refresh action; the global policy keeps it fast.
    if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FULL_REFRESH &&
        mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    
    const bool nextTriggered =
        touchNext ||
        (usePressForPageTurn
             ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                mappedInput.wasPressed(MappedInputManager::Button::Right))
             : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                mappedInput.wasReleased(MappedInputManager::Button::Right)));

    if (!prevTriggered && !nextTriggered) {
      return;
    }

    // 吸收物理长按跳章后紧随而来的松手翻页事件（否则跳章后会额外翻一页）
    if (physicalSkipAbsorb) {
      physicalSkipAbsorb = false;
      return;
    }

    // any botton press when at end of the book goes back to the last page
    if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      updateRequired = true;
      return;
    }

    // BT虚拟按键长按跳章：计数连续收到的同向按键事件来模拟长按。
    // 双重保护：
    //   1) BT_IDLE_RESET_MS=950ms：比 BT 自动重复间隔(~750ms)稍大，比人工点按间隔(>1s)稍小
    //   2) BT_MIN_HOLD_MS=2000ms：序列总时长超过2秒才允许跳章
    if (SETTINGS.longPressChapterSkip && !isPhysicalButton) {
      constexpr uint8_t  BT_HOLD_THRESHOLD = 3;
      constexpr unsigned long BT_IDLE_RESET_MS = 950;
      constexpr unsigned long BT_MIN_HOLD_MS   = 2000;

      static uint8_t       btPressCount  = 0;
      static bool          btLastIsPrev  = false;
      static bool          btSkipFired   = false;
      static unsigned long btLastPressMs = 0;
      static unsigned long btSeqStartMs  = 0;

      const unsigned long now = millis();
      const unsigned long gap = (btLastPressMs == 0) ? 9999UL : (now - btLastPressMs);
      const bool isNewSeq = (btLastPressMs == 0) ||
                            (btLastIsPrev != prevTriggered) ||
                            gap > BT_IDLE_RESET_MS;

      if (isNewSeq) {
        btPressCount = 1;
        btLastIsPrev = prevTriggered;
        btSkipFired  = false;
        btSeqStartMs = now;
        Serial.printf("[%lu] [BT-SKIP] New seq: dir=%s gap=%lums count=1\n",
                      now, prevTriggered ? "prev" : "next", gap);
      } else {
        btPressCount++;
        Serial.printf("[%lu] [BT-SKIP] Continue: dir=%s gap=%lums count=%d/%d elapsed=%lums (need>=%lums)\n",
                      now, prevTriggered ? "prev" : "next", gap,
                      btPressCount, BT_HOLD_THRESHOLD,
                      now - btSeqStartMs, BT_MIN_HOLD_MS);
      }
      btLastPressMs = now;

      if (btSkipFired) {
        Serial.printf("[%lu] [BT-SKIP] Suppressed (already fired)\n", now);
        return;
      }

      if (btPressCount >= BT_HOLD_THRESHOLD && (now - btSeqStartMs) >= BT_MIN_HOLD_MS) {
        btSkipFired = true;
        Serial.printf("[%lu] [BT-SKIP] *** CHAPTER SKIP TRIGGERED count=%d elapsed=%lums ***\n",
                      now, btPressCount, now - btSeqStartMs);
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        nextPageNumber = 0;
        currentSpineIndex = prevTriggered ? currentSpineIndex - 1 : currentSpineIndex + 1;
        section.reset();
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }
      // count < 阈值 或 时长 < 2s → 正常翻页，继续下流
    }

    // No current section, attempt to rerender the book
    if (!section) {
      updateRequired = true;
      return;
    }

    if (prevTriggered) {
      const int step = isLandscapeDualPage() ? 2 : 1;
      if (section->currentPage - step >= 0) {
        section->currentPage -= step;
        dualRightPage = -1; dualNextLeft = true;  // 手动翻页重置双页状态
      } else if (section->currentPage > 0) {
        section->currentPage = 0;
        dualRightPage = -1; dualNextLeft = true;
      } else {
        // We don't want to delete the section mid-render, so grab the semaphore
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
        xSemaphoreGive(renderingMutex);
        dualRightPage = -1; dualNextLeft = true;
      }
      updateRequired = true;
    } else {
      const int step = isLandscapeDualPage() ? 2 : 1;
      if (section->currentPage + step < section->pageCount) {
        section->currentPage += step;
        dualRightPage = -1; dualNextLeft = true;  // 手动翻页重置双页状态
      } else if (section->currentPage < section->pageCount - 1) {
        section->currentPage = section->pageCount - 1;
        dualRightPage = -1; dualNextLeft = true;
      } else {
        // We don't want to delete the section mid-render, so grab the semaphore
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
        xSemaphoreGive(renderingMutex);
        dualRightPage = -1; dualNextLeft = true;
      }
      updateRequired = true;
    }
    //暂不启用，易起冲突，后面修改
  }else if (state == EPUBState::SETTING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ) {
      Serial.printf("[%lu] [ERS] Long press detected, entering reading\n", millis());
      state = EPUBState::READING;
      skipNextButtonCheck = true;
      SETTINGS.saveToFile();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      Serial.printf("[%lu] [ERS] 进入左边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Left+=5;
      section.reset();
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      Serial.printf("[%lu] [ERS] 进入右边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Right+=5;
      section.reset();
      xSemaphoreGive(renderingMutex);

      updateRequired = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      Serial.printf("[%lu] [ERS] 进入上边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Top+=5;
      section.reset();
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      Serial.printf("[%lu] [ERS] 进入下边距设置\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      SETTINGS.screenMargin_Bottom+=5;
      Serial.printf("[%lu] [ERS] Bottom为%d\n", millis(), SETTINGS.screenMargin_Bottom);
      section.reset();
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
    }

  } else if (state == EPUBState::END_OF_BOOK_CONFIRM) {
    // 处理书籍阅读结束时的删除确认
    // 左键/上键：选择"取消"
    // 右键/下键：选择"确定"
    // 菜单键 (Confirm)：确认选择

    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      deleteConfirmSelected = false;
      updateRequired = true;
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      deleteConfirmSelected = true;
      updateRequired = true;
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // 用户确认选择
      if (deleteConfirmSelected) {
        // 选择"确定"，删除文件
        Serial.printf("[%lu] [ERS] User confirmed delete, removing file: %s\n", millis(), epub->getPath().c_str());

        // 删除 epub 文件（可能是缓存的转换文件或原始 epub）
        const std::string epubPath = epub->getPath();
        bool deleted = SdMan.remove(epubPath.c_str());

        // 清理 EPUB 缓存目录（包含缩略图、解析缓存等）
        if (epub) {
          Serial.printf("[%lu] [ERS] Clearing EPUB cache directory: %s\n", millis(), epub->getCachePath().c_str());
          epub->clearCache();
        }

        // 检查并删除原始 txt 文件（如果从 txt 转换而来）
        if (!originalSourcePath.empty()) {
          // 检查是否是 txt 文件（通过扩展名判断，不区分大小写）
          if (StringUtils::checkFileExtension(originalSourcePath, ".txt")) {
            Serial.printf("[%lu] [ERS] Also removing original txt file: %s\n", millis(), originalSourcePath.c_str());
            bool txtDeleted = SdMan.remove(originalSourcePath.c_str());
            if (txtDeleted) {
              Serial.printf("[%lu] [ERS] Original txt file deleted successfully\n", millis());
            } else {
              Serial.printf("[%lu] [ERS] Failed to delete original txt file\n", millis());
            }
          }
        }

        if (deleted) {
          Serial.printf("[%lu] [ERS] File deleted successfully\n", millis());
          GUI.drawPopup(renderer, "文件已删除");
        } else {
          Serial.printf("[%lu] [ERS] Failed to delete file\n", millis());
          GUI.drawPopup(renderer, "删除失败");
        }
        renderer.displayBuffer();
        
        // 返回主页
        pendingGoHome = true;
      } else {
        // 选择"取消"，返回到书的最后一页
        Serial.printf("[%lu] [ERS] User cancelled delete\n", millis());
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        currentSpineIndex = epub->getSpineItemsCount() - 1;
        nextPageNumber = UINT16_MAX;
        section.reset();
        xSemaphoreGive(renderingMutex);
        state = EPUBState::READING;
        updateRequired = true;
      }
    }
  }
}

void EpubReaderActivity::onReaderMenuBack(const uint8_t orientation) {
  exitActivity();
  // Reload fonts from SD in case the user changed font settings in the menu.
  EpdFontLoader::loadFontsFromSd(renderer);
  // Apply the user-selected orientation when the menu is dismissed.
  // This ensures the menu can be navigated without immediately rotating the screen.
  applyOrientation(orientation);
  // Apply auto page turn settings from SETTINGS
  applyAutoPageTurnSettings();
  // Save current page before resetting section so we return to the same page.
  if (section) {
    nextPageNumber = section->currentPage;
  }
  // Reset section to re-render with potentially new font/settings
  section.reset();
  updateRequired = true;
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  // Delegate to the float version for a single code path.
  percent = clampPercent(percent);
  jumpToPercent(static_cast<float>(percent) / 100.0f);
}

// Translate a normalized percentage (0.0-1.0) into a spine index plus a
// normalized position within that spine. This preserves full float precision
// from sync callbacks (KOReader, JianGuo) without integer truncation.
void EpubReaderActivity::jumpToPercent(float normalizedPercent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Clamp to valid range.
  if (normalizedPercent < 0.0f) {
    normalizedPercent = 0.0f;
  } else if (normalizedPercent > 1.0f) {
    normalizedPercent = 1.0f;
  }

  // Convert normalized percentage directly into a byte position.
  size_t targetSize = static_cast<size_t>(static_cast<float>(bookSize) * normalizedPercent);
  if (normalizedPercent >= 1.0f) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so renderScreen() reloads and repositions on the target spine.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  currentSpineIndex = targetSpineIndex;
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
  xSemaphoreGive(renderingMutex);
}

void EpubReaderActivity::enterReaderMenu(EpubReaderMenuActivity::MenuLayer layer) {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;
  float bookProgress = 0.0f;
  if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  const std::string title = epub ? epub->getTitle() : std::string();
  enterNewActivity(new EpubReaderMenuActivity(
      this->renderer, this->mappedInput, title, currentPage, totalPages, bookProgressPercent,
      SETTINGS.orientation, [this](const uint8_t orientation) { onReaderMenuBack(orientation); },
      [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }, layer));
}

void EpubReaderActivity::enterChapterSelector() {
  const int currentP = section ? section->currentPage : 0;
  const int totalP = section ? section->pageCount : 0;
  const int spineIdx = currentSpineIndex;
  const std::string path = epub ? epub->getPath() : std::string();
  enterNewActivity(new EpubReaderChapterSelectionActivity(
      this->renderer, this->mappedInput, epub, path, spineIdx, currentP, totalP,
      [this] {
        exitActivity();
        updateRequired = true;
      },
      [this](const int newSpineIndex) {
        if (currentSpineIndex != newSpineIndex) {
          currentSpineIndex = newSpineIndex;
          nextPageNumber = 0;
          section.reset();
        }
        exitActivity();
        updateRequired = true;
      },
      [this](const int newSpineIndex, const int newPage) {
        if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
          currentSpineIndex = newSpineIndex;
          nextPageNumber = newPage;
          section.reset();
        }
        exitActivity();
        updateRequired = true;
      }));
}

void EpubReaderActivity::enterPercentSheet() {
  float bookProgress = 0.0f;
  if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  enterNewActivity(new EpubReaderPercentSelectionActivity(
      renderer, mappedInput, initialPercent,
      [this](const int percent) {
        jumpToPercent(percent);
        exitActivity();
        updateRequired = true;
      },
      [this]() {
        exitActivity();
        updateRequired = true;
      },
      [this](int toolbarHit) {
        if (toolbarHit == 1) return;
        exitActivity();
        if (toolbarHit == 0) {
          enterChapterSelector();
        } else if (toolbarHit == 2) {
          enterReaderMenu(EpubReaderMenuActivity::MenuLayer::STYLE);
        } else if (toolbarHit == 3) {
          enterReaderMenu(EpubReaderMenuActivity::MenuLayer::MORE);
        }
      }));
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterChapterSelector();
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterPercentSheet();
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      // Defer go home to avoid race condition with display task
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      if (epub) {
        // 2. BACKUP: Read current progress
        // We use the current variables that track our position
        uint16_t backupSpine = currentSpineIndex;
        uint16_t backupPage = section->currentPage;
        uint16_t backupPageCount = section->pageCount;

        section.reset();
        // 3. WIPE: Clear the cache directory
        epub->clearCache();

        // 4. RESTORE: Re-setup the directory and rewrite the progress file
        epub->setupCacheDir();

        saveProgress(backupSpine, backupPage, backupPageCount);
      }
      xSemaphoreGive(renderingMutex);
      // Defer go home to avoid race condition with display task
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        exitActivity();
        enterNewActivity(new KOReaderSyncActivity(
            renderer, mappedInput, epub, epub->getPath(), currentSpineIndex, currentPage, totalPages,
            [this]() {
              // On cancel - defer exit to avoid use-after-free
              pendingSubactivityExit = true;
            },
            [this](float percentage) {
              // On sync complete - jump to the percentage position for precise intra-chapter positioning
              jumpToPercent(percentage);
              pendingSubactivityExit = true;
            }));
        xSemaphoreGive(renderingMutex);
      } else {
        // 关键修复：没有配置KOReader时，提示用户去系统设置
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        GUI.drawPopup(renderer, "请先在系统设置中配置KOReader");
        renderer.displayBuffer();
        xSemaphoreGive(renderingMutex);
        delay(500);  // 显示0.5秒
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNCY: {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        exitActivity();
        enterNewActivity(new JianGuoSyncActivity(
            renderer, mappedInput, epub, epub->getPath(),currentSpineIndex,
            [this]() {
              exitActivity();
              updateRequired = true;
            },
            [this](float percentage) {
              Serial.printf("[%lu] [JG] 同步完成，百分比：%.1f%%\n", millis(), percentage * 100);
              // Use jumpToPercent for precise positioning (including intra-chapter)
              if (percentage > 0.0f) {
                jumpToPercent(percentage);
              }
              exitActivity();
              updateRequired = true;
            }
          
          ));
        xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new EpubReaderSettingsActivity(
          renderer, mappedInput, [this] {
            // 返回阅读器后重载当前章节以应用新设置（字体、边距等影响排版）
            exitActivity();
            section.reset();
            // 设置期间 autoPageTurnEnabled 可能变化：重新对表，避免旧定时器
            // 回到阅读器后立即触发一次翻页（与 onReaderMenuBack 一致）。
            applyAutoPageTurnSettings();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }

    case EpubReaderMenuActivity::MenuAction::ADD_BOOKMARK: {
      // Extract text from current page for bookmark title
      std::string pageText;
      if (section) {
        int savedPage = section->currentPage;
        auto page = section->loadPageFromSectionFile();
        section->currentPage = savedPage;
        if (page) {
          for (const auto& elem : page->elements) {
            if (elem->getTag() == TAG_PageLine) {
              auto* pageLine = static_cast<PageLine*>(elem.get());
              std::string lineText = pageLine->getText();
              if (!lineText.empty()) {
                if (!pageText.empty()) pageText += " ";
                pageText += lineText;
                // We only need first few characters, stop early
                if (pageText.size() > 60) break;
              }
            }
          }
        }
      }

      // Calculate current book progress
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress =
            static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
      }
      const int percentInt = static_cast<int>(bookProgress * 100.0f + 0.5f);
      const int currentPage = section ? section->currentPage + 1 : 0;

      // Extract first 5 non-space UTF-8 characters for title prefix
      std::string titlePrefix;
      if (!pageText.empty()) {
        const unsigned char* ptr = reinterpret_cast<const unsigned char*>(pageText.c_str());
        int charCount = 0;
        while (*ptr && charCount < 5) {
          if (*ptr == ' ') {
            ptr++;  // skip space without counting
            continue;
          }
          const unsigned char* charStart = ptr;
          utf8NextCodepoint(&ptr);
          titlePrefix.append(reinterpret_cast<const char*>(charStart), ptr - charStart);
          charCount++;
        }
      }

      // Build bookmark title: "前5字...（第X页, XX%）"
      std::string bmTitle;
      if (!titlePrefix.empty()) {
        bmTitle = titlePrefix + "...";
      }
      bmTitle += "（第" + std::to_string(currentPage) + "页, " + std::to_string(percentInt) + "%）";

      Bookmark bm;
      bm.title = bmTitle;
      bm.percentage = bookProgress;
      bm.spineIndex = currentSpineIndex;
      bm.page = section ? section->currentPage : 0;
      bm.timestamp = static_cast<int64_t>(millis());
      bm.bookPath = epub->getPath();
      bm.bookTitle = epub->getTitle();

      // Try to get real timestamp
      {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec > 1704067200) {  // After 2024-01-01
          bm.timestamp = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
        }
      }

      const std::string bookMd5 = BookmarkStore::calculateBookMd5(epub->getPath());
      BookmarkStore::addBookmark(bookMd5, bm);

      // Show confirmation popup and defer menu exit to avoid use-after-free
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      GUI.drawPopup(renderer, "书签已添加");
      renderer.displayBuffer();
      xSemaphoreGive(renderingMutex);
      pendingSubactivityExit = true;
      break;
    }

    case EpubReaderMenuActivity::MenuAction::BOOKMARK_MANAGER: {
      const std::string bookMd5 = BookmarkStore::calculateBookMd5(epub->getPath());
      auto bookmarks = BookmarkStore::loadBookmarks(bookMd5);

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new BookmarkManagerActivity(
          renderer, mappedInput, std::move(bookmarks),
          [this]() {
            // Go back
            exitActivity();
            updateRequired = true;
          },
          [this](float percentage) {
            // Jump to bookmark
            jumpToPercent(percentage);
            exitActivity();
            updateRequired = true;
          },
          [this, bookMd5](int index) {
            // Delete bookmark
            BookmarkStore::deleteBookmark(bookMd5, index);
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }

  }
}

/**
 * Rolling half-page render: blends the top half of the next page with the bottom half of the
 * current page to create a smooth reading experience. The reader can finish reading the bottom
 * of the current page while already seeing the top of the next page.
 *
 * Sequence:
 *   1. Render page N+1 → saveTopHalfBuffer() saves its top half (24KB)
 *   2. Render page N   → restoreTopHalfToFrame() pastes N+1's top into the top half
 *   3. A thin divider is drawn at the midpoint to mark the page boundary
 *   4. Status bar + fast display
 *
 * Returns false (caller falls through to normal render) at chapter boundaries.
 */
bool EpubReaderActivity::renderRollingHalfTurn(const int top, const int right, const int bottom, const int left) {
  if (!section || !epub) return false;

  const int curPage = section->currentPage;
  const int nextPage = curPage + 1;

  // Chapter boundary: can't show next page without loading a new section (expensive).
  // The loop() will handle this as a normal page turn on the second timer tick.
  if (nextPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Rolling: at last page of section, skipping half-page render\n", millis());
    return false;
  }

  // SD card temp file for half buffer (saves 24KB RAM!)
  const std::string halfBufferPath = epub->getCachePath() + "/halfbuf.bin";

  // 1. Load next page (N+1) by temporarily changing currentPage
  section->currentPage = nextPage;
  auto nextPageObj = section->loadPageFromSectionFile();
  section->currentPage = curPage;

  if (!nextPageObj) {
    Serial.printf("[%lu] [ERS] Rolling: failed to load next page\n", millis());
    return false;
  }

  // 2. Render next page into framebuffer (BW only for speed; no grayscale)
  renderer.clearScreen();
  if (SETTINGS.ReadingScreenEnabled) {
    renderPngSleepScreen(renderer);
  }
  nextPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);

  // 3. Save the logical top half of this render to SD card (saves 24KB RAM!)
  if (!renderer.saveTopHalfToSd(halfBufferPath.c_str())) {
    Serial.printf("[%lu] [ERS] Rolling: failed to save top-half to SD\n", millis());
    return false;
  }

  // 4. Load and render current page (N) for its bottom half content
  auto curPageObj = section->loadPageFromSectionFile();
  if (!curPageObj) {
    return false;
  }

  renderer.clearScreen();
  if (SETTINGS.ReadingScreenEnabled) {
    renderPngSleepScreen(renderer);
  }
  curPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);

  // 5. Paste N+1's top half back from SD card
  renderer.restoreTopHalfFromSd(halfBufferPath.c_str());

  // 6. Draw double-line separator at the logical screen midpoint
  //    Structure (6px total band):
  //      midY-3 : 1px black line
  //      midY-2 to midY+5 : 8px white gap (clears any text pixels)
  //      midY+6 : 1px black line
  const int midY = renderer.getScreenHeight() / 2;
  const int lineX1 = left;
  const int lineX2 = renderer.getScreenWidth() - right;
  renderer.drawLine(lineX1, midY - 3, lineX2, midY - 3, 1, true);   // top black
  renderer.drawLine(lineX1, midY - 2, lineX2, midY - 2, 8, false);  // 8px white gap
  renderer.drawLine(lineX1, midY + 6, lineX2, midY + 6, 1, true);   // bottom black

  // 7. Status bar and display — use FAST_REFRESH for smooth rolling experience
  //    Rolling mode triggers 2 FAST_REFRESHs per logical page turn (half-turn + full-turn),
  //    so we decrement the counter here to ensure cleanup triggers at the right frequency.
  renderStatusBar(right, bottom, top, left);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  // Decrement counter: rolling mode effectively does 2 refreshes per page turn
  if (pagesUntilFullRefresh > 0) {
    pagesUntilFullRefresh--;
  }

  // 8. Grayscale anti-aliasing passes — mirrors renderContents grayscale logic.
  //    Using SD card for half buffer means we only need 48KB RAM for BW buffer (not 72KB)!
  //
  //    Normal path:
  //      Render N+1 gray → save top half to SD → render N gray → restore top half from SD →
  //      copyGrayscale*Buffers → displayGrayBuffer
  //      Result: full composite AA (top half = N+1, bottom half = N)
  //
  //    Fallback path (SD save/restore fails):
  //      Render N gray → clearLogicalTopHalf → copyGrayscale*Buffers
  //      The top half is zeroed (= LUT entry "00": no waveform, pixel keeps BW state)
  //      so the BW layer's N+1 content shows through unchanged in the top half.
  //      Result: bottom half = N with AA, top half = N+1 in BW only.
  
  const bool bwBufferStored = renderer.storeBwBuffer();
  if (bwBufferStored && SETTINGS.textAntiAliasing) {
    // --- LSB pass ---
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    nextPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);
    if (renderer.saveTopHalfToSd(halfBufferPath.c_str())) {
      // Normal: composite N+1 (top) + N (bottom)
      renderer.clearScreen(0x00);
      curPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);
      renderer.restoreTopHalfFromSd(halfBufferPath.c_str());
    } else {
      // Fallback: AA only on bottom half (N); top half stays at BW state via LUT "00"
      Serial.printf("[%lu] [ERS] Rolling AA LSB: SD save failed, falling back to bottom-half only\n", millis());
      renderer.clearScreen(0x00);
      curPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);
      renderer.clearLogicalTopHalf();
    }
    renderer.copyGrayscaleLsbBuffers();

    // --- MSB pass ---
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    nextPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);
    if (renderer.saveTopHalfToSd(halfBufferPath.c_str())) {
      // Normal: composite N+1 (top) + N (bottom)
      renderer.clearScreen(0x00);
      curPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);
      renderer.restoreTopHalfFromSd(halfBufferPath.c_str());
    } else {
      // Fallback: AA only on bottom half (N)
      Serial.printf("[%lu] [ERS] Rolling AA MSB: SD save failed, falling back to bottom-half only\n", millis());
      renderer.clearScreen(0x00);
      curPageObj->render(renderer, SETTINGS.getReaderFontId(), left, top);
      renderer.clearLogicalTopHalf();
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
  } else {
    // storeBwBuffer failed or AA disabled - skip grayscale rendering to keep BW/RED RAM consistent
    if (!bwBufferStored && SETTINGS.textAntiAliasing) {
      Serial.printf("[%lu] [ERS] Rolling: storeBwBuffer failed (OOM), skipping AA to prevent faded text\n", millis());
    }
  }

  // Progress is still anchored to page N (reader is finishing it)
  saveProgress(currentSpineIndex, curPage, section->pageCount);

  Serial.printf("[%lu] [ERS] Rolling: rendered half-page (top=page %d, bottom=page %d)\n",
                millis(), nextPage, curPage);
  return true;
}

void EpubReaderActivity::applyAutoPageTurnSettings() {
  // Always reset rolling state when applying settings
  rollingMode = false;
  rollingHalfTurned = false;

  if (!SETTINGS.autoPageTurnEnabled) {
    automaticPageTurnActive = false;
    Serial.printf("[%lu] [ERS] Auto page turn disabled\n", millis());
    return;
  }

  // Set up auto page turn from SETTINGS
  lastPageTurnTime = millis();
  pageTurnDuration = SETTINGS.autoPageTurnInterval * 1000UL;
  automaticPageTurnActive = true;
  rollingMode = (SETTINGS.autoPageTurnMode == 1);  // 0=full, 1=rolling
  
  if (rollingMode) {
    Serial.printf("[%lu] [ERS] Rolling auto-turn enabled: interval %ds\n",
                  millis(), SETTINGS.autoPageTurnInterval);
  } else {
    Serial.printf("[%lu] [ERS] Auto page turn enabled: interval %ds\n",
                  millis(), SETTINGS.autoPageTurnInterval);
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  // Legacy function - now just calls applyAutoPageTurnSettings
  // Keep for backward compatibility
  applyAutoPageTurnSettings();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  // Persist the selection so the reader keeps the new orientation on next launch.
  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();

  // Update renderer orientation to match the new logical coordinate system.
  applyReaderOrientation(renderer, SETTINGS.orientation);

  // Reset section to force re-layout in the new orientation.
  section.reset();
  xSemaphoreGive(renderingMutex);
}

void EpubReaderActivity::displayTaskLoop() {
  while (true) {
    if (subActivity) {
      updateRequired = false;
      vTaskDelay(20 / portTICK_PERIOD_MS);
      continue;
    }
    if (updateRequired) {
      updateRequired = false;
      if (subActivity) {
        vTaskDelay(20 / portTICK_PERIOD_MS);
        continue;
      }
      // 加锁保证渲染过程独占
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      APP_STATE.isRenderComplete = false; // 标记渲染开始
      renderScreen(); // 执行核心渲染逻辑
      APP_STATE.isRenderComplete = true;  // 标记渲染完成（包括 saveProgress）
      xSemaphoreGive(renderingMutex);     // 释放锁
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // 降低轮询频率，节省资源
  }
}

// TODO: Failure handling
void EpubReaderActivity::renderScreen() {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen with delete confirmation dialog
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    // Set state to END_OF_BOOK_CONFIRM if not already
    if (state != EPUBState::END_OF_BOOK_CONFIRM) {
      state = EPUBState::END_OF_BOOK_CONFIRM;
      deleteConfirmSelected = false;  // Reset selection to "Cancel"
    }
    
    renderer.clearScreen();
    
    // 禁用自动翻页（已到书末）
    automaticPageTurnActive = false;
    rollingMode = false;
    rollingHalfTurned = false;
    
    // Draw title
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 300, "已读完本书，是否删除？", true, EpdFontFamily::BOLD);
    
    // Draw options - highlight selected one
    const int optionY = 450;
    const int optionSpacing = 120;
    const char* cancelText = "取消";
    const char* deleteText = "确定";
    
    // Calculate positions to center the options
    int cancelWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, cancelText);
    int deleteWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, deleteText);
    int totalWidth = cancelWidth + deleteWidth + optionSpacing;
    int startX = (renderer.getScreenWidth() - totalWidth) / 2;
    
    // Draw "取消" (Cancel) option
    if (!deleteConfirmSelected) {
      // Selected - draw with inverted colors
      int cancelHeight = renderer.getLineHeight(UI_12_FONT_ID) + 10;
      renderer.fillRect(startX, optionY - 5, cancelWidth + 20, cancelHeight, true);
      M4UiText::draw(renderer, UI_12_FONT_ID, startX + 10, optionY, cancelText, false, EpdFontFamily::BOLD);
    } else {
      M4UiText::draw(renderer, UI_12_FONT_ID, startX, optionY, cancelText, true, EpdFontFamily::REGULAR);
    }
    
    // Draw "确定" (Delete) option
    int deleteX = startX + cancelWidth + optionSpacing;
    if (deleteConfirmSelected) {
      // Selected - draw with inverted colors
      int deleteHeight = renderer.getLineHeight(UI_12_FONT_ID) + 10;
      renderer.fillRect(deleteX - 10, optionY - 5, deleteWidth + 20, deleteHeight, true);
      M4UiText::draw(renderer, UI_12_FONT_ID, deleteX, optionY, deleteText, false, EpdFontFamily::BOLD);
    } else {
      M4UiText::draw(renderer, UI_12_FONT_ID, deleteX, optionY, deleteText, true, EpdFontFamily::REGULAR);
    }
    
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding
  // 1. 先获取屏幕原始可视边距（无任何叠加）
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  auto metrics = UITheme::getInstance().getMetrics();

  // 2. 先处理状态栏留出空间（确保阅读内容不与状态栏重叠）
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    const bool isOnlyProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
    if (showProgressBar) {
      const int textLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      // 从屏幕底部向上算局所需的总占用空间:
      // 仅进度条: 8(底部间距) + 4(进度条高度) + 5(顶部间距) = 17px
      // 其他进度条模式: 5(底部间距) + 4(进度条高度) + 1(文字间距) + textLineHeight + 5(顶部间距)
      const int required = isOnlyProgressBar
          ? (progressBarBottomGapOnly + metrics.bookProgressBarHeight + statusBarTopGap)
          : (progressBarBottomGap + metrics.bookProgressBarHeight + progressBarTextGap + textLineHeight + statusBarTopGap);
      if (orientedMarginBottom < required) {
        orientedMarginBottom = required;
      }
    } else {
      // FULL / NO_PROGRESS 等模式: 只有文字，无进度条
      // 需要: 底部间距(5px) + 文字行高 + 顶部间距(5px)
      const int textLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int required = progressBarBottomGap + textLineHeight + statusBarTopGap;
      if (orientedMarginBottom < required) {
        orientedMarginBottom = required;
      }
    }
  }

  // 3. 再叠加用户设置的所有边距（此时Bottom边距不会被抵消）
  orientedMarginTop += SETTINGS.screenMargin_Top;
  orientedMarginLeft += SETTINGS.screenMargin_Left;
  orientedMarginRight += SETTINGS.screenMargin_Right;
  orientedMarginBottom += SETTINGS.screenMargin_Bottom; 

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    Serial.printf("[%lu] [ERS] Loading file: %s, index: %d\n", millis(), filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    if (isLandscapeDualPage()) {
      // 展开内侧间距 = 装订线间隙 + 左页右边距 + 右页左边距（均取用户设置内侧边距值）
      viewportWidth = (viewportWidth - DUAL_PAGE_GUTTER - SETTINGS.screenMargin_Right - SETTINGS.screenMargin_Left) / 2;
    }
    const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    Serial.printf("[%lu] [ERS] Calculated viewport: %dx%d (Screen: %dx%d, Margins L:%d R:%d T:%d B:%d)\n", millis(),
                  viewportWidth, viewportHeight, renderer.getScreenWidth(), renderer.getScreenHeight(),
                  orientedMarginLeft, orientedMarginRight, orientedMarginTop, orientedMarginBottom);

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled,SETTINGS.wordSpacing,SETTINGS.firstlineintented, SETTINGS.embeddedStyle,
                                  static_cast<bool>(SETTINGS.chinesePunctWidth), static_cast<bool>(SETTINGS.epubShowImages))) {
      Serial.printf("[%lu] [ERS] Cache not found, building...\n", millis());

      const auto popupFn = [this]() { GUI.drawPopup(renderer, "加载章节中..."); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.wordSpacing, SETTINGS.firstlineintented, SETTINGS.embeddedStyle,
                                      static_cast<bool>(SETTINGS.chinesePunctWidth), static_cast<bool>(SETTINGS.epubShowImages), popupFn)) {
        Serial.printf("[%lu] [ERS] Failed to persist page data to SD\n", millis());
        section.reset();
        return;
      }
    } else {
      Serial.printf("[%lu] [ERS] Cache found, skipping build...\n", millis());
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      // Use rounding (+0.5f) to avoid float truncation losing a page.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount) + 0.5f);
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();
    //加背景
    if(SETTINGS.ReadingScreenEnabled){
      Serial.printf("[%lu] [ERS] 壁纸屏幕开启，渲染壁纸屏幕\n", millis());
      renderPngSleepScreen(renderer);
    }


  if (section->pageCount == 0) {
    Serial.printf("[%lu] [ERS] Empty chapter at spine %d, auto-skipping\n", millis(), currentSpineIndex);
    section.reset();
    // Auto-skip forward; if already at the last spine item, try backward
    if (currentSpineIndex < epub->getSpineItemsCount() - 1) {
      currentSpineIndex++;
      nextPageNumber = 0;
    } else if (currentSpineIndex > 0) {
      currentSpineIndex--;
      nextPageNumber = UINT16_MAX;
    } else {
      // Single empty chapter book - show fallback
      M4UiText::drawCentered(renderer, UI_12_FONT_ID, 300, "Empty chapter", true, EpdFontFamily::BOLD);
      renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginTop, orientedMarginLeft);
      renderer.displayBuffer();
      return;
    }
    return renderScreen();
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 300, "Out of bounds", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom,orientedMarginTop, orientedMarginLeft);
    renderer.displayBuffer();
    automaticPageTurnActive = false;  // 禁用自动翻页
    rollingMode = false;
    rollingHalfTurned = false;
    return;
  }

  // 卷帘半屏翻转：显示下一页开头 + 当前页结尾的混合画面
  // 无论成功或失败（章节边界），都直接返回，等待第二阶段定时器处理
  if (isLandscapeDualPage()) {
    // 横屏双页模式：加载左右两页并渲染
    // 初始化右页状态（section 重新加载或指针超范围时）
    if (dualRightPage < 0 || dualRightPage >= section->pageCount) {
      dualRightPage = (section->currentPage + 1 < section->pageCount) ? section->currentPage + 1 : section->currentPage;
      dualNextLeft = true;
    }
    auto leftPage = section->loadPageFromSectionFile();
    if (!leftPage) {
      Serial.printf("[%lu] [ERS] Failed to load left page from SD - clearing section cache\n", millis());
      section->clearCache();
      section.reset();
      return renderScreen();
    }
    std::unique_ptr<Page> rightPage;
    if (dualRightPage != section->currentPage && dualRightPage < section->pageCount) {
      int savedPage = section->currentPage;
      section->currentPage = dualRightPage;
      rightPage = section->loadPageFromSectionFile();
      section->currentPage = savedPage;
    }
    renderDualContents(std::move(leftPage), std::move(rightPage),
                       orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  } else {
    if (automaticPageTurnActive && rollingMode && rollingHalfTurned) {
      renderRollingHalfTurn(orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
      return;
    }

    {
      auto p = section->loadPageFromSectionFile();
      if (!p) {
        Serial.printf("[%lu] [ERS] Failed to load page from SD - clearing section cache\n", millis());
        section->clearCache();
        section.reset();
        return renderScreen();
      }
      const auto start = millis();
      renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
      Serial.printf("[%lu] [ERS] Rendered page in %dms\n", millis(), millis() - start);
    }
  }
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
}



void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  // Ensure cache directory exists before writing (may have been cleared)
  epub->setupCacheDir();

  FsFile f;
  if (SdMan.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    Serial.printf("[ERS] Progress saved: Chapter %d, Page %d\n", spineIndex, currentPage);
  } else {
    Serial.printf("[ERS] Could not save progress!\n");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  // Keep image pages on the fast BW pass; grayscale rendering follows through
  // the driver’s fast-gray path without a legacy full/half waveform.
  bool forceFullRefresh = page->hasImages() && SETTINGS.textAntiAliasing;
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar(orientedMarginRight, orientedMarginBottom,orientedMarginTop, orientedMarginLeft);
  // Apply dark mode inversion (reading area only, before displaying)
  if (SETTINGS.epubDarkMode) {
    renderer.invertScreen();
  }
  if (!forceFullRefresh && pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::READER_CLEANUP_REFRESH, HalDisplay::READER_BODY_CONTEXT);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh--;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  const bool bwBufferStored = renderer.storeBwBuffer();

  // grayscale rendering
  // 仅当 BW 缓冲区保存成功时才执行灰度渲染，否则跳过以防止残影
  if (bwBufferStored && SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // restore the bw data
  renderer.restoreBwBuffer();
}

void EpubReaderActivity::renderDualContents(std::unique_ptr<Page> leftPage, std::unique_ptr<Page> rightPage,
                                            const int mt, const int mr, const int mb, const int ml) {
  const int totalVPW = renderer.getScreenWidth() - ml - mr;
  // 内侧间距 = 装订线间隙 + 左页右边距 + 右页左边距
  const int innerGutter = DUAL_PAGE_GUTTER + SETTINGS.screenMargin_Right + SETTINGS.screenMargin_Left;
  const int halfVPW = (totalVPW - innerGutter) / 2;
  const int rightLeft = ml + halfVPW + innerGutter;

  // 渲染左右两页
  leftPage->render(renderer, SETTINGS.getReaderFontId(), ml, mt);
  if (rightPage) {
    rightPage->render(renderer, SETTINGS.getReaderFontId(), rightLeft, mt);
  }

  // 竖向分隔线：居中于左页右边距结束后的 DUAL_PAGE_GUTTER 中心
  const int sepX = ml + halfVPW + SETTINGS.screenMargin_Right + DUAL_PAGE_GUTTER / 2;
  const int lineY1 = mt;
  const int lineY2 = renderer.getScreenHeight() - mb;
  renderer.drawLine(sepX - 3, lineY1, sepX - 3, lineY2, 1, true);   // left black
  renderer.drawLine(sepX - 2, lineY1, sepX - 2, lineY2, 8, false);  // 8px white gap
  renderer.drawLine(sepX + 6, lineY1, sepX + 6, lineY2, 1, true);   // right black

  renderStatusBar(mr, mb, mt, ml);

  if (SETTINGS.epubDarkMode) {
    renderer.invertScreen();
  }

  // 显示 BW
  const bool forceFullRefresh = (leftPage->hasImages() || (rightPage && rightPage->hasImages())) && SETTINGS.textAntiAliasing;
  if (!forceFullRefresh && pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::READER_CLEANUP_REFRESH, HalDisplay::READER_BODY_CONTEXT);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh--;
  }

  // AA 灰度渲染
  const bool bwBufferStored = renderer.storeBwBuffer();
  if (bwBufferStored && SETTINGS.textAntiAliasing) {
    // LSB pass
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    leftPage->render(renderer, SETTINGS.getReaderFontId(), ml, mt);
    if (rightPage) {
      rightPage->render(renderer, SETTINGS.getReaderFontId(), rightLeft, mt);
    }
    renderer.copyGrayscaleLsbBuffers();

    // MSB pass
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    leftPage->render(renderer, SETTINGS.getReaderFontId(), ml, mt);
    if (rightPage) {
      rightPage->render(renderer, SETTINGS.getReaderFontId(), rightLeft, mt);
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  renderer.restoreBwBuffer();
}

void EpubReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginTop, const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  // determine visible status bar elements
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBookProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                   SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  // 章节名/时间显示：只有当状态栏模式包含章节名时才生效
  const bool shouldShowChapterOrTime = showChapterTitle && SETTINGS.showTimeInsteadOfChapter;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Position status bar elements using absolute coordinates from screen bottom
  const auto screenHeight = renderer.getScreenHeight();
  const int textLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const bool hasProgressBar = showBookProgressBar || showChapterProgressBar;
  // 进度条顶部Y坐标: 屏幕底部向上5px
  const int progressBarTopY = screenHeight - progressBarBottomGap - metrics.bookProgressBarHeight;
  // 文字顶部Y坐标:
  //   - 有进度条: 进度条上方1px再向上一个行高
  //   - 无进度条(FULL/NO_PROGRESS): 屏幕底部上方5px再向上一个行高
  const int textY = hasProgressBar
      ? (progressBarTopY - progressBarTextGap - textLineHeight)
      : (screenHeight - progressBarBottomGap - textLineHeight);
  int progressTextWidth = 0;

  // Calculate progress in book
  const float sectionChapterProg = static_cast<float>(section->currentPage) / section->pageCount;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Hide percentage when progress bar is shown to reduce clutter
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", section->currentPage + 1, section->pageCount,
               bookProgress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", section->currentPage + 1, section->pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showBookProgressBar) {
    if (SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR) {
      // "仅进度条" mode: keep bar at hardware bottom (current style)
      GUI.drawReadingProgressBar(renderer, static_cast<size_t>(bookProgress));
    } else {
      // "完整+进度条" mode: 进度条紧贴屏幕底部(5px间距)
      const int barMaxWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
      const int barWidth = barMaxWidth * static_cast<int>(bookProgress) / 100;
      renderer.fillRect(orientedMarginLeft, progressBarTopY, barWidth, metrics.bookProgressBarHeight, true);
    }
  }

  if (showChapterProgressBar) {
    // "完整+章节条" mode: 进度条紧贴屏幕底部(5px间距)
    const float chapterProgress =
        (section->pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / section->pageCount) * 100 : 0;
    const int barMaxWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int barWidth = barMaxWidth * static_cast<int>(chapterProgress) / 100;
    renderer.fillRect(orientedMarginLeft, progressBarTopY, barWidth, metrics.bookProgressBarHeight, true);
  }

  if (showBattery) {
    GUI.drawBattery(renderer, Rect{orientedMarginLeft + 1, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  // ── 右上角不再显示时间，clockTotalWidth 始终为 0 ──
  int clockTotalWidth = 0;

  if (showChapterTitle || shouldShowChapterOrTime) {
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;

    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + clockTotalWidth + 30;
    const int titleMarginRight = progressTextWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

    std::string title;
    int titleWidth;
    if (automaticPageTurnActive) {
      // 自动翻页模式激活时，显示指示
      if (rollingMode) {
        title = L(Str::kHalfScreenPaging);
      } else {
        title = L(Str::kAutoPaging);
      }
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    } else if (SETTINGS.showTimeInsteadOfChapter) {
      // 显示带图标的时间（居中）
      title = getClockTimeString();
      
      // 如果时间无效（返回 "--:--"），则回退到显示章节名
      if (title == "--:--") {
        // 时间无效，回退到章节名逻辑
        if (tocIndex == -1) {
          title = "Unnamed";
          titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
        } else {
          const auto tocItem = epub->getTocItem(tocIndex);
          title = tocItem.title;
          titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
          if (titleWidth > availableTitleSpace) {
            // Not enough space to center on the screen, center it within the remaining space instead
            availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
            titleMarginLeftAdjusted = titleMarginLeft;
          }
          if (titleWidth > availableTitleSpace) {
            title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
            titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
          }
        }
        // 绘制章节名（不带图标）
        renderer.drawText(SMALL_FONT_ID,
                          titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                          title.c_str());
        return;
      }
      
      const int iconTotalWidth = clockIconSize + clockIconTextSpacing + renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      
      // 如果总宽度超过可用空间，截断文本
      if (iconTotalWidth > availableTitleSpace) {
        int maxTextWidth = availableTitleSpace - clockIconSize - clockIconTextSpacing;
        if (maxTextWidth > 0) {
          title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), maxTextWidth);
        } else {
          title = "";
        }
      }
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      
      // 计算整体内容的起始 X 坐标，使其居中
      int totalContentWidth = clockIconSize + clockIconTextSpacing + titleWidth;
      int startX = titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - totalContentWidth) / 2;
      
      // 绘制时钟图标
      const int fontHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int clockCX = startX + clockIconRadius;
      const int clockCY = textY + fontHeight / 2;
      drawClockIcon(renderer, clockCX, clockCY, clockIconRadius);
      
      // 绘制时间文本
      renderer.drawText(SMALL_FONT_ID, startX + clockIconSize + clockIconTextSpacing, textY, title.c_str());
      return; // 绘制完成，直接返回
    } else if (tocIndex == -1) {
      title = "Unnamed";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        // Not enough space to center on the screen, center it within the remaining space instead
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      title.c_str());
  }
}




void EpubReaderActivity::renderPngSleepScreen(GfxRenderer& renderer) const {

  auto dir = SdMan.open("/bizhi");
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid PNG files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      // 判断png后缀（对齐txtpng的文件格式判断）
      std::string ext = filename.substr(filename.length() - 4);
      for (auto& c : ext) c = tolower(c);
      if (ext != ".png") {
        Serial.printf("[%lu] [SLP] Skipping non-.png file name: %s\n", millis(), name);
        file.close();
        continue;
      }
    
      // 验证PNG文件是否有效（对齐txtpng的文件打开校验）
      ImageDimensions pngDim;
      if (!PngToFramebufferConverter::getDimensionsStatic("/bizhi/" + filename, pngDim)) {
        Serial.printf("[%lu] [SLP] Skipping invalid PNG file: %s\n", millis(), name);
        file.close();
        continue;
      }
      files.emplace_back(filename);
      file.close();
    }
    Serial.printf("[%lu] [SLP] Found %d valid PNG files\n", millis(), files.size());
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // 只选一个
      auto randomFileIndex = 0;
      
      
      Serial.printf("[%lu] [SLP] randomFileIndex: %d\n", millis(), randomFileIndex);
      const auto filename = "/bizhi/" + files[randomFileIndex];
      Serial.printf("[%lu] [SLP] Randomly loading: %s\n", millis(), filename.c_str());
      delay(100);
    
      // 配置PNG渲染参数
      RenderConfig renderConfig;
      renderConfig.x = 0;                
      renderConfig.y = 0;                
      renderConfig.maxWidth = 480;       
      renderConfig.maxHeight = 800;      
      renderConfig.useDithering = true;
      renderConfig.cachePath = "";
    
      // 解码并渲染PNG
      PngToFramebufferConverter pngConverter;
      if (pngConverter.decodeToFramebuffer(filename, renderer, renderConfig)) {
        // ========== 对齐txtpng的绘制完成后无额外操作，仅刷新 ==========
        //renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        //delay(200); // 给屏幕刷新时间
        dir.close();
        Serial.printf("[%lu] [SLP] Png draw completed (mode: %d)\n", millis(), renderer.getRenderMode());
        return;
      } else {
        Serial.printf("[%lu] [SLP] Failed to render PNG: %s\n", millis(), filename.c_str());
      }
    }
  }
  if (dir) dir.close();


  // 无有效PNG文件，保持底层显示（对齐txtpng的失败处理）
  Serial.printf("[%lu] [SLP] No valid PNG file, keep default screen\n", millis());
}

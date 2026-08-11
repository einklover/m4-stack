#include <Arduino.h>
#include <EpdFontLoader.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <SDCardManager.h>
#include <SPI.h>
#ifdef OMIT_FONTS
// Murphy M4: APP1 is 7.14 MiB — full notosans_13_bold CJK tables do not fit.
// Built-in UI subset covers all shipped zh-CN/zh-TW I18n strings + ASCII.
// Full-book CJK still uses SD /fonts/*.epdfont via EpdFontLoader (not .cpfont).
#include <builtinFonts/m4_ui_cjk_13.h>
#include <builtinFonts/m4_ui_cjk_16.h>
#else
#include <builtinFonts/all.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstring>

#ifdef M4_QEMU_BUILD
#include <hal/adc_hal_common.h>

// Espressif QEMU 9.2.2 never raises the ESP32-S3 ADC calibration-done bit,
// so IDF's pre-app_main calibration constructor otherwise waits forever.
extern "C" uint32_t __wrap_adc_hal_self_calibration(adc_unit_t, adc_atten_t, bool) {
  return 0;
}

// Volatile keeps the complete application reachable in the QEMU ELF while
// defaulting the current emulator to its modeled-peripheral boundary.
static volatile bool gM4QemuScreenMode = true;
#endif

#include <HalPowerManager.h>
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "I18n.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/MyLibraryActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/apps/AppListActivity.h"
#include "activities/apps/AppInstallActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "apps/M4xInstaller.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "managers/FontCacheManager.h"
#include "activities/browser/JianGuoBrowserActivity.h"
#include "activities/browser/DataCapsuleBrowserActivity.h"
#include "activities/reader/BookmarkNotesActivity.h"

#include <BluetoothHIDManager.h>
#include "util/ButtonNavigator.h"

#ifdef CROSSPOINT_MURPHY_M4
#include <FrontlightManager.h>
#include <esp_attr.h>
#include <esp32-hal.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "util/M4FrontlightPolicy.h"
#include "util/M4FontPolicy.h"
#include "util/M4SdStatus.h"
#endif

#ifdef CROSSPOINT_MURPHY_M4
#include "debug/M4SerialDebugBridge.h"
#include "apps/M4xRegistry.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "activities/apps/AppRuntimeActivity.h"
#include "activities/apps/NativeAppActivity.h"
#endif

#ifdef CROSSPOINT_X3
#include "TiltPageTurnDetector.h"
#include "DS3231RTC.h"
#else
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include "WifiCredentialStore.h"
#endif


HalDisplay display;
HalGPIO gpio;
GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
Activity* currentActivity;

#ifdef CROSSPOINT_MURPHY_M4
static M4SerialDebug::Bridge gM4DebugBridge;
static std::string gDebugActiveAppId;

namespace {

constexpr uint32_t kM4PanicCaptureMagic = 0x4D345043u;  // "M4PC"

struct M4PanicCapture {
  uint32_t magic;
  uint32_t core;
  uintptr_t pc;
  uint32_t backtraceLen;
  uint32_t providerStage;
  uint32_t backtrace[60];
  char reason[64];
};

RTC_NOINIT_ATTR M4PanicCapture gM4PanicCapture;

void captureM4Panic(arduino_panic_info_t* info, void*) {
  if (!info) return;
  gM4PanicCapture.magic = 0;
  gM4PanicCapture.core = static_cast<uint32_t>(info->core);
  gM4PanicCapture.pc = reinterpret_cast<uintptr_t>(info->pc);
  gM4PanicCapture.backtraceLen = info->backtrace_len > 60u ? 60u : info->backtrace_len;
  gM4PanicCapture.providerStage = M4NativeProviderHeavyGate::diagnosticStage();
  for (uint32_t i = 0; i < gM4PanicCapture.backtraceLen; ++i) {
    gM4PanicCapture.backtrace[i] = info->backtrace[i];
  }
  size_t n = 0;
  if (info->reason) {
    while (n + 1 < sizeof(gM4PanicCapture.reason) && info->reason[n]) {
      gM4PanicCapture.reason[n] = info->reason[n];
      ++n;
    }
  }
  gM4PanicCapture.reason[n] = '\0';
  gM4PanicCapture.magic = kM4PanicCaptureMagic;
}

void flushM4PanicCapture() {
  if (gM4PanicCapture.magic != kM4PanicCaptureMagic) return;
  constexpr char kDir[] = "/apps_data/com.weread.client/logs";
  constexpr char kPath[] = "/apps_data/com.weread.client/logs/panic_last.log";
  SdMan.mkdir("/apps_data", true);
  SdMan.mkdir("/apps_data/com.weread.client", true);
  SdMan.mkdir(kDir, true);
  FsFile f = SdMan.open(kPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;

  char line[160];
  const int n = std::snprintf(line, sizeof(line),
                              "panic reason=%s core=%u pc=0x%08lx frames=%u provider_stage=0x%08x\n",
                              gM4PanicCapture.reason, static_cast<unsigned>(gM4PanicCapture.core),
                              static_cast<unsigned long>(gM4PanicCapture.pc),
                              static_cast<unsigned>(gM4PanicCapture.backtraceLen),
                              static_cast<unsigned>(gM4PanicCapture.providerStage));
  if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
  for (uint32_t i = 0; i < gM4PanicCapture.backtraceLen; ++i) {
    const int m = std::snprintf(line, sizeof(line), "%u:0x%08x\n", static_cast<unsigned>(i),
                                static_cast<unsigned>(gM4PanicCapture.backtrace[i]));
    if (m > 0) f.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(m));
  }
  f.sync();
  f.close();
  gM4PanicCapture.magic = 0;
}

}  // namespace
#endif

#ifdef CROSSPOINT_MURPHY_M4
FrontlightManager frontlightManager;
static uint8_t appliedFrontlightBrightness = UINT8_MAX;
static uint8_t appliedFrontlightWarmth = UINT8_MAX;

static void applyFrontlightSettings(bool force = false) {
  const auto levels = M4FrontlightPolicy::normalize(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth);
  SETTINGS.frontlightBrightness = levels.brightness;
  SETTINGS.frontlightWarmth = levels.warmth;
  if (!force && levels.brightness == appliedFrontlightBrightness && levels.warmth == appliedFrontlightWarmth) {
    return;
  }
  frontlightManager.setColorTemperature(levels.warmth);
  frontlightManager.setBrightness(levels.brightness);
  appliedFrontlightBrightness = levels.brightness;
  appliedFrontlightWarmth = levels.warmth;
  Serial.printf("[%lu] [M4-LIGHT] Applied frontlight brightness=%u%% warmth=%u%% off=%d dual=%d\n", millis(),
                static_cast<unsigned>(levels.brightness), static_cast<unsigned>(levels.warmth),
                M4FrontlightPolicy::isOff(levels.brightness) ? 1 : 0,
                frontlightManager.hasColorTemperature() ? 1 : 0);
}

// Terminal boot summary for scripted serial audit (no secrets).
// touch_cfg = controller configured; touch_ready = settle+stream probe (may still be pending at early print).
static void printM4BootSummary(bool psramOk, bool dispOk, bool sdOk, bool touchCfg, bool touchReady, bool fontOk,
                               bool lightOk) {
  Serial.printf("[%lu] [M4-RC1] BOOT_SUMMARY ver=" CROSSPOINT_VERSION
                " psram=%s disp=%s sd=%s touch_cfg=%s touch_ready=%s font=%s light=%s\n",
                millis(), psramOk ? "ok" : "fail", dispOk ? "ok" : "fail", sdOk ? "ok" : "fail",
                touchCfg ? "yes" : "no", touchReady ? "yes" : "pending", fontOk ? "ok" : "fail",
                lightOk ? "ok" : "fail");
}
#endif

#ifdef CROSSPOINT_X3
TiltPageTurnDetector tiltDetector;
#endif

#ifndef CROSSPOINT_X3
// NTP 同步状态机
enum class NtpSyncState {
  IDLE,           // 未开始
  PENDING,        // 等待在主线程执行
  CONNECTING,     // 正在连接WiFi
  SYNCING,        // 正在同步NTP
  CLEANING_UP     // 正在清理WiFi
};

// Cross-task handoff (delay task → main loop): must be atomic, not volatile.
// volatile only suppresses optimization; it does not provide acquire/release.
std::atomic<NtpSyncState> ntpSyncState{NtpSyncState::IDLE};
std::atomic<bool> ntpSyncDone{false};

/**
 * 启动NTP时间同步（仅X4平台）
 * 每次开机只同步一次，由 APP_STATE.ntpSyncedThisBoot 控制
 * 异步执行，不会阻塞主线程
 */
void syncNtpTime() {
  // 如果已经同步过，跳过
  if (APP_STATE.ntpSyncedThisBoot) {
    Serial.printf("[%lu] [NTP] Already synced this boot, skipping\n", millis());
    return;
  }
  
  // 检查是否开启了自动同步时间
  if (!SETTINGS.autoSyncTimeOnBoot) {
    Serial.printf("[%lu] [NTP] Auto-sync on boot is disabled, skipping\n", millis());
    APP_STATE.ntpSyncedThisBoot = true;  // 标记为已完成，避免重复检查
    return;
  }
  
  // 检查可用内存，如果内存不足则跳过NTP同步
  const uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[%lu] [NTP] Free heap: %lu bytes\n", millis(), freeHeap);
  
  // 如果可用内存少于100KB，跳过NTP同步（WiFi需要大量内存）
  if (freeHeap < 100000) {
    Serial.printf("[%lu] [NTP] Insufficient memory (%lu bytes), skipping NTP sync\n", millis(), freeHeap);
    APP_STATE.ntpSyncedThisBoot = true;
    return;
  }
  
  // 标记正在同步，避免重复启动
  APP_STATE.ntpSyncedThisBoot = true;
  
  // 延迟10秒后再开始NTP同步，确保阅读器完全加载
  // 使用延时任务而不是状态机，避免复杂性
  Serial.printf("[%lu] [NTP] Will schedule NTP sync after 10 seconds delay\n", millis());
  
  // 创建一个延迟任务，10秒后设置状态为PENDING
  xTaskCreate(
    [](void* param) {
      vTaskDelay(10000 / portTICK_PERIOD_MS);  // 延迟10秒
      
      // 再次检查内存
      const uint32_t currentFreeHeap = ESP.getFreeHeap();
      if (currentFreeHeap < 80000) {
        Serial.printf("[%lu] [NTP] Still insufficient memory (%lu bytes), aborting\n", millis(), currentFreeHeap);
        vTaskDelete(nullptr);
        return;
      }
      
      // 设置状态为PENDING，由主线程处理
      ntpSyncState.store(NtpSyncState::PENDING, std::memory_order_release);
      Serial.printf("[%lu] [NTP] NTP sync scheduled in main thread\n", millis());
      
      vTaskDelete(nullptr);
    },
    "NtpDelayTask",
    2048,  // 较小的栈
    nullptr,
    0,     // 最低优先级
    nullptr
  );
}
#endif

// Fonts - 只保留 NotoSans 系统字体 (full CJK) unless OMIT_FONTS (M4 size budget)
#ifndef OMIT_FONTS
EpdFont notosans12RegularFont(&notosans_13_bold);
EpdFont notosans12BoldFont(&notosans_13_bold);
EpdFont notosans12ItalicFont(&notosans_13_bold);
EpdFont notosans12BoldItalicFont(&notosans_13_bold);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_13_bold);
EpdFont notosans14BoldFont(&notosans_13_bold);
EpdFont notosans14ItalicFont(&notosans_13_bold);
EpdFont notosans14BoldItalicFont(&notosans_13_bold);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_13_bold);
EpdFont notosans16BoldFont(&notosans_13_bold);
EpdFont notosans16ItalicFont(&notosans_13_bold);
EpdFont notosans16BoldItalicFont(&notosans_13_bold);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_13_bold);
EpdFont notosans18BoldFont(&notosans_13_bold);
EpdFont notosans18ItalicFont(&notosans_13_bold);
EpdFont notosans18BoldItalicFont(&notosans_13_bold);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

EpdFont smallFont(&notosans_13_bold);
EpdFont ui10RegularFont(&notosans_13_bold);
EpdFont ui10BoldFont(&notosans_13_bold);
EpdFont ui12RegularFont(&notosans_13_bold);
EpdFont ui12BoldFont(&notosans_13_bold);
#else
// Size-safe Chinese UI subset (I18n charset). Reader full-CJK needs SD epdfont.
EpdFont notosans12RegularFont(&m4_ui_cjk_13);
EpdFont notosans12BoldFont(&m4_ui_cjk_13);
EpdFont notosans12ItalicFont(&m4_ui_cjk_13);
EpdFont notosans12BoldItalicFont(&m4_ui_cjk_13);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&m4_ui_cjk_13);
EpdFont notosans14BoldFont(&m4_ui_cjk_13);
EpdFont notosans14ItalicFont(&m4_ui_cjk_13);
EpdFont notosans14BoldItalicFont(&m4_ui_cjk_13);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&m4_ui_cjk_16);
EpdFont notosans16BoldFont(&m4_ui_cjk_16);
EpdFont notosans16ItalicFont(&m4_ui_cjk_16);
EpdFont notosans16BoldItalicFont(&m4_ui_cjk_16);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&m4_ui_cjk_16);
EpdFont notosans18BoldFont(&m4_ui_cjk_16);
EpdFont notosans18ItalicFont(&m4_ui_cjk_16);
EpdFont notosans18BoldItalicFont(&m4_ui_cjk_16);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

EpdFont smallFont(&m4_ui_cjk_13);
EpdFont ui10RegularFont(&m4_ui_cjk_13);
EpdFont ui10BoldFont(&m4_ui_cjk_13);
EpdFont ui12RegularFont(&m4_ui_cjk_13);
EpdFont ui12BoldFont(&m4_ui_cjk_13);
#endif  // OMIT_FONTS

EpdFontFamily smallFontFamily(&smallFont);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);


// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Deferred delete: activity objects are not freed immediately in exitActivity()
// to prevent use-after-free when exitActivity() is called from within the
// activity's own call stack (e.g., reader back button callback chain).
static Activity* deferredDeleteActivity = nullptr;

void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    // If there's a previously deferred activity, delete it now (it's no longer
    // in any call stack since we've completed at least one full loop iteration).
    if (deferredDeleteActivity) {
      delete deferredDeleteActivity;
    }
    deferredDeleteActivity = currentActivity;
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;
  currentActivity->onEnter();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  // If long-press-to-boot is disabled, any press length is sufficient to boot.
  // This is independent of the shortPwrBtn (power button function) setting,
  // because the device is not yet on when this check runs.
  if (!SETTINGS.longPressBoot) {
    return;
  }

  // Long press required to boot (2 seconds), regardless of shortPwrBtn setting.
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  constexpr uint16_t bootPressDuration = 2000;  // Always 2 seconds to boot
  const uint16_t calibratedPressDuration =
      (calibration < bootPressDuration) ? bootPressDuration - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() < calibratedPressDuration);
    abort = gpio.getHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    gpio.startDeepSleep();
  }
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  //等待渲染完成
  uint32_t waitStart = millis();
  const uint32_t MAX_WAIT_TIME = 5000; // 最多等5秒
  while (!APP_STATE.isRenderComplete) {
    Serial.printf("[%lu] [MAIN] Waiting for main render to complete...\n", millis());
    vTaskDelay(100 / portTICK_PERIOD_MS); // 每100ms检查一次
    
    // 超时保护：避免卡死
    if (millis() - waitStart > MAX_WAIT_TIME) {
      Serial.printf("[%lu] [MAIN] Wait timeout, proceed with PNG render\n", millis());
      break;
    }
  }
  //原逻辑

  APP_STATE.lastSleepFromReader = currentActivity && currentActivity->isReaderActivity();
  APP_STATE.saveToFile();

  //bluetooth
  try {
    auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) {
      Serial.printf("[%lu] [SLP] Disabling Bluetooth before deep sleep\n", millis());
      btMgr.disable();
    }
  } catch (...) {
    Serial.printf("[%lu] [SLP] Could not disable Bluetooth\n", millis());
  }


  exitActivity();
  enterNewActivity(new SleepActivity(renderer, mappedInputManager));

  display.deepSleep();
  Serial.printf("[%lu] [   ] Power button press calibration value: %lu ms\n", millis(), t2 - t1);
  Serial.printf("[%lu] [   ] Entering deep sleep (display will be powered off by battery latch).\n", millis());

#ifdef CROSSPOINT_MURPHY_M4
  frontlightManager.off();
  Serial.printf("[%lu] [M4-LIGHT] Frontlight off for deep sleep\n", millis());
#endif
  gpio.startDeepSleep();
}


void onGoHome();
void onGoHomeAnimated(bool animateEntry, int animationDirection);
void onGoToMyLibraryWithPath(const std::string& path);
void onGoToRecentBooks();
void onGoToReader(const std::string& initialEpubPath, const std::string& originalSourcePath = "") {
  exitActivity();
  enterNewActivity(
      new ReaderActivity(renderer, mappedInputManager, initialEpubPath, onGoHome, onGoToMyLibraryWithPath, originalSourcePath));
}

void onGoToFileTransfer() {
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome));
}

// USB debug entry point: use the already-prepared STA link and show the same
// native transfer page without requiring a second touch/menu selection.
void onGoToFileTransferUsb() {
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome, true));
}

void onGoToSettings() {
  exitActivity();
  enterNewActivity(new SettingsActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToApps() {
  exitActivity();
  enterNewActivity(new AppListActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToMyLibrary() {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome,
                                         [](const std::string& path, const std::string& originalSourcePath) { onGoToReader(path, originalSourcePath); }));
}

void onGoToRecentBooks() {
  exitActivity();
  enterNewActivity(new RecentBooksActivity(renderer, mappedInputManager, onGoHome,
                                           [](const std::string& path, const std::string& originalSourcePath) { onGoToReader(path, originalSourcePath); }));
}

void onGoToMyLibraryWithPath(const std::string& path) {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome,
                                         [](const std::string& p, const std::string& originalSourcePath) { onGoToReader(p, originalSourcePath); }, path));
}

void onGoToBrowser() {
  exitActivity();
  enterNewActivity(new OpdsBookBrowserActivity(renderer, mappedInputManager, onGoHome));
}
void onGoToJianGuoYun() {
  exitActivity();
  enterNewActivity(new JianGuoBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToDataCapsule() {
  exitActivity();
  enterNewActivity(new DataCapsuleBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToBookmarkNotes() {
  exitActivity();
  enterNewActivity(new BookmarkNotesActivity(
      renderer, mappedInputManager,
      onGoHome,
      [](const std::string& bookPath, float percentage) {
        // Store pending percentage in APP_STATE so EpubReaderActivity can apply it after loading
        APP_STATE.pendingBookmarkPercent = percentage;
        onGoToReader(bookPath);
      }));
}

void onGoHomeAnimated(const bool animateEntry, const int animationDirection) {
  exitActivity();
  enterNewActivity(new HomeActivity(renderer, mappedInputManager,
                                    [](const std::string& path, const std::string& originalSourcePath) { onGoToReader(path, originalSourcePath); },
                                    onGoToMyLibrary, onGoToRecentBooks,
                                    onGoToSettings, onGoToFileTransfer, onGoToBrowser, onGoToJianGuoYun,
                                    onGoToDataCapsule, onGoToBookmarkNotes, onGoToApps,
                                    animateEntry, animationDirection));
#ifdef CROSSPOINT_MURPHY_M4
  gDebugActiveAppId.clear();
#endif
}

void onGoHome() {
  // Button/keyboard home is intentionally immediate. Touch home gestures call
  // onGoHomeAnimated() with the system animation settings instead.
  onGoHomeAnimated(false, 0);
}

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  Serial.printf("[%lu] [M4-DISP] Display initialized\n", millis());
  // Mandatory IDs (NOTOSANS_*, UI_*, SMALL) always registered so SYSTEM_FONT /
  // UI drawing never no-ops. OMIT_FONTS builds use the M4 UI CJK subset.
  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
#ifdef OMIT_FONTS
  Serial.printf("[%lu] [M4-FONT] Registered mandatory IDs with m4_ui_cjk subset "
                "(UI strings offline; full-book CJK needs SD /fonts/*.epdfont)\n",
                millis());
#else
  Serial.printf("[%lu] [FONT] Builtin NotoSans full CJK registered\n", millis());
#endif
  Serial.printf("[%lu] [FONT] Fonts setup complete\n", millis());
}


void setup() {
    // force serial for debugging
    // USB CDC RX queue is small by default; the debug bridge sends control
    // frames up to ~800B (Waveform Lab LUT uploads) and the e-ink main loop
    // can be blocked by BUSY for 100+ ms, so a tiny queue drops frames.
    // Enlarge before begin() (runtime, the compile-time macro is ignored).
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    Serial.setRxBufferSize(8192);
#endif
    Serial.begin(115200);
#ifdef CROSSPOINT_MURPHY_M4
    set_arduino_panic_handler(captureM4Panic, nullptr);
#endif
    delay(500);
    Serial.printf("[%lu] [M4-RC1] setup() start ver=" CROSSPOINT_VERSION "\n", millis());

    // ========== 设置时区（东八区）==========
    // 必须在 RTC 读取和 NTP 同步之前设置，否则 mktime/localtime_r 会按 UTC 处理时间
    // CST-8 表示中国标准时间（China Standard Time），偏移 UTC+8
    setenv("TZ", "CST-8", 1);
    tzset();
    Serial.flush();

    t1 = millis();

    gpio.begin();
    powerManager.begin();  // X3 inits I2C fuel gauge; X4 is a no-op for ADC

#ifdef CROSSPOINT_MURPHY_M4
    bool m4PsramOk = false;
    bool m4SdOk = false;
    bool m4TouchOk = false;
    bool m4FontOk = false;
    bool m4LightOk = false;
    bool m4DispOk = false;
    {
      const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      const size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
      m4PsramOk = psramTotal > 0;
      Serial.printf("[%lu] [M4-PSRAM] free=%u total=%u\n", millis(),
                    static_cast<unsigned>(psramFree), static_cast<unsigned>(psramTotal));
      if (psramTotal == 0) {
        Serial.printf("[%lu] [M4-PSRAM] WARNING: PSRAM not detected; continuing with internal RAM only\n",
                      millis());
      }
    }
    frontlightManager.begin();
    // Early-boot default so SD/display errors remain readable before settings load.
    {
      const auto early = M4FrontlightPolicy::earlyBootDefaults();
      frontlightManager.setColorTemperature(early.warmth);
      frontlightManager.setBrightness(early.brightness);
      m4LightOk = true;
      Serial.printf("[%lu] [M4-LIGHT] early-boot frontlight brightness=%u%% warmth=%u%%\n", millis(),
                    static_cast<unsigned>(early.brightness), static_cast<unsigned>(early.warmth));
    }
    // Variables above are function-scoped for this #ifdef region of setup().
#endif

    // Only start serial if USB connected
    if (gpio.isUsbConnected()) {
        Serial.begin(115200);
        // Wait up to 3 seconds for Serial to be ready to catch early logs
        unsigned long start = millis();
        while (!Serial && (millis() - start) < 3000) {
            delay(10);
        }
    }

#ifdef M4_QEMU_BUILD
    if (gM4QemuScreenMode) {
      // QEMU has no Murphy SSD1677/SDMMC devices. Exercise the real UI renderer,
      // then let FreeInkDisplay export the committed 1bpp frame over UART.
      setupDisplayAndFonts();
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, mappedInputManager,
                                                     "Murphy QEMU ready",
                                                     EpdFontFamily::BOLD));
      Serial.printf("[%lu] [M4-QEMU] screen bridge ready\n", millis());
      return;
    }
#endif

    // SD Card Initialization (classified stage/detail; never format user media).
    // Second chance: cold boot / post-flash can leave the rail unsettled; a short
    // delay + full re-begin often succeeds even when the first probe reported no_card
    // despite a physical card being present.
    if (!SdMan.begin()) {
        Serial.printf("[%lu] [M4-SD] first begin failed stage=%s code=%s detail=%s — retry in 400ms\n", millis(),
                      SdMan.lastStage(), SdMan.lastCodeName(), SdMan.lastDetail());
        delay(400);
        if (!SdMan.begin()) {
          Serial.printf("[%lu] [M4-SD] ERROR stage=%s code=%s detail=%s\n", millis(), SdMan.lastStage(),
                        SdMan.lastCodeName(), SdMan.lastDetail());
#ifdef CROSSPOINT_MURPHY_M4
          // Touch not stream-ready at this point; report configured only.
          printM4BootSummary(m4PsramOk, false, false, gpio.hasTouch(), gpio.isTouchStreamReady(), false, m4LightOk);
#endif
          setupDisplayAndFonts();
          exitActivity();
          // "no_card" is often a bus/power false negative, not an empty slot.
          char sdMsg[120];
          const char* code = SdMan.lastCodeName();
          if (code && strcmp(code, "no_card") == 0) {
            snprintf(sdMsg, sizeof(sdMsg),
                     "SD init fail (no_card)\nReseat card & reboot\n%s", SdMan.lastDetail());
          } else if (code && strcmp(code, "unsupported_fs") == 0) {
            snprintf(sdMsg, sizeof(sdMsg), "SD: bad filesystem\nUse FAT32\n%s", SdMan.lastDetail());
          } else if (code && (strcmp(code, "mount_timeout") == 0 || strcmp(code, "sector_timeout") == 0)) {
            snprintf(sdMsg, sizeof(sdMsg), "SD: bus timeout\nReseat & reboot\n%s", SdMan.lastDetail());
          } else {
            snprintf(sdMsg, sizeof(sdMsg), "SD: %s/%s\n%s", SdMan.lastStage(), code ? code : "?",
                     SdMan.lastDetail());
          }
          enterNewActivity(new FullScreenMessageActivity(renderer, mappedInputManager, sdMsg, EpdFontFamily::BOLD));
          return;
        }
        Serial.printf("[%lu] [M4-SD] second begin succeeded after retry\n", millis());
    }
#ifdef CROSSPOINT_MURPHY_M4
    // Read-only capability probe: root list + optional settings file if present.
    if (!SdMan.capabilityProbe("/.crosspoint/settings.json")) {
      Serial.printf("[%lu] [M4-SD] ERROR capability_probe stage=%s code=%s detail=%s\n", millis(), SdMan.lastStage(),
                    SdMan.lastCodeName(), SdMan.lastDetail());
      printM4BootSummary(m4PsramOk, false, false, gpio.hasTouch(), gpio.isTouchStreamReady(), false, m4LightOk);
      setupDisplayAndFonts();
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, mappedInputManager, "SD: io_failure",
                                                     EpdFontFamily::BOLD));
      return;
    }
    m4SdOk = true;
    Serial.printf("[%lu] [M4-SD] mounted ok part=%d fatType=%u sectors=%llu stage=%s code=%s\n", millis(),
                  SdMan.lastMountedPart(), static_cast<unsigned>(SdMan.lastFatType()),
                  static_cast<unsigned long long>(SdMan.lastSectorCount()), SdMan.lastStage(),
                  SdMan.lastCodeName());
    flushM4PanicCapture();
#else
    Serial.printf("[%lu] [M4-SD] mounted ok\n", millis());
#endif

    SETTINGS.loadFromFile();
#ifdef CROSSPOINT_MURPHY_M4
    applyFrontlightSettings(true);
    m4LightOk = true;
#endif
    KOREADER_STORE.loadFromFile();
    READING_STATS.loadFromFile();
    UITheme::getInstance().reload();

    ButtonNavigator::setMappedInputManager(mappedInputManager);
    
    // 开机时强制不自动启动蓝牙，避免内存不足导致 crash
    // 用户需在蓝牙设置中手动开启；阅读器仅在蓝牙已启用时才自动连接
    SETTINGS.bluetoothEnabled = 0;

    // 设置蓝牙按钮注入回调（仅需设置一次，在开机时完成）
    // 蓝牙的自动连接在 EpubReaderActivity::onEnter() 中同步执行
    try {
        auto& btMgr = BluetoothHIDManager::getInstance();
        btMgr.setButtonInjector([](uint8_t buttonIndex) {
            gpio.injectButtonPress(buttonIndex);
        });
    } catch (...) {
        Serial.printf("[%lu] [MAIN] 蓝牙按钮注入回调设置失败\n", millis());
    }

#ifdef CROSSPOINT_X3
    // ========== 晃动翻页传感器初始化（仅在功能开启时） ==========
    tiltDetector.setActionCallback([](TiltPageTurnDetector::Action action) {
        switch (action) {
            case TiltPageTurnDetector::Action::PREV_PAGE:
                gpio.injectButtonPress(SETTINGS.frontButtonLeft);
                Serial.println("[TILT] -> Prev page");
                break;
            case TiltPageTurnDetector::Action::NEXT_PAGE:
                gpio.injectButtonPress(SETTINGS.frontButtonRight);
                Serial.println("[TILT] -> Next page");
                break;
            case TiltPageTurnDetector::Action::TOGGLE_AUTO_PAGE_TURN:
                SETTINGS.autoPageTurnEnabled = SETTINGS.autoPageTurnEnabled ? 0 : 1;
                SETTINGS.saveToFile();
                Serial.printf("[TILT] -> Auto page turn: %s\n",
                              SETTINGS.autoPageTurnEnabled ? "ON" : "OFF");
                break;
            case TiltPageTurnDetector::Action::OPEN_MENU:
                gpio.injectButtonPress(SETTINGS.frontButtonBack);
                Serial.println("[TILT] -> Open menu");
                break;
            case TiltPageTurnDetector::Action::AUTO_ROTATE:
                // 自动旋转：由 EpubReaderActivity 在 loop 中轮询 detectedOrientation 处理
                // 这里不直接操作，因为方向切换需要 reader 内部的 applyOrientation
                Serial.printf("[ROTATE] -> Orientation change detected: %d\n", 
                              tiltDetector.getDetectedOrientation());
                break;
            default:
                break;
        }
    });
    if (SETTINGS.tiltPageTurnEnabled || SETTINGS.tapPageTurnEnabled || SETTINGS.autoRotateEnabled) {
        tiltDetector.begin(X3_I2C_SDA, X3_I2C_SCL);
        Serial.printf("[%lu] [TILT] 传感器初始化完成 (ready=%d, tilt=%d, tap=%d, rotate=%d)\n", 
                      millis(), tiltDetector.isReady(), SETTINGS.tiltPageTurnEnabled, 
                      SETTINGS.tapPageTurnEnabled, SETTINGS.autoRotateEnabled);
    } else {
        Serial.printf("[%lu] [TILT] 晃动/敲击/旋转均未启用，跳过传感器初始化\n", millis());
    }

    // ========== I2C 总线扫描结果（已确认设备）==========
    // 0x55 - BQ27220 (Fuel Gauge)
    // 0x68 - DS3231/DS1307 (RTC)
    // 0x6B - QMI8658 (IMU)
    // 0x7E - Unknown

    // ========== 从 RTC 恢复系统时钟 ==========
    if (DS3231RTC::syncSystemFromRTC()) {
        struct tm timeinfo;
        time_t now = time(nullptr);
        localtime_r(&now, &timeinfo);
        Serial.printf("[%lu] [RTC] System time: %04d-%02d-%02d %02d:%02d:%02d\n",
                      millis(), timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.printf("[%lu] [RTC] Failed to sync system clock from RTC\n", millis());
    }

#endif  // CROSSPOINT_X3

    // ... 后面的代码完全保留不变 ...
    
    switch (gpio.getWakeupReason()) {
        case HalGPIO::WakeupReason::PowerButton:
            // Woken by power button: verify the user held long enough
            verifyPowerButtonDuration();
            break;
        case HalGPIO::WakeupReason::AfterFlash:
            break;
        case HalGPIO::WakeupReason::AfterUSBPower:
            break;
        default:
            break;
    }

    Serial.printf("[%lu] [   ] Starting CrossPoint version " CROSSPOINT_VERSION "\n", millis());

    // 初始化字体缓存管理器（Flash 分区 mmap）
    FontCacheManager::begin();
    Serial.printf("[%lu] [DBG] FontCacheManager initialized\n", millis());
    Serial.flush();

    setupDisplayAndFonts();
    Serial.printf("[%lu] [M4-DISP] setupDisplayAndFonts done\n", millis());
    Serial.flush();
#ifdef CROSSPOINT_MURPHY_M4
    m4DispOk = true;
    // Poll through settle window so stream_ready can become true before summary.
    {
      const unsigned long settleDeadline = millis() + 3200;
      while (millis() < settleDeadline && !gpio.isTouchStreamReady()) {
        gpio.update();
        delay(20);
      }
      m4TouchOk = gpio.isTouchStreamReady();
      Serial.printf("[%lu] [M4-TOUCH] configured=%d stream_ready=%d\n", millis(), gpio.hasTouch() ? 1 : 0,
                    m4TouchOk ? 1 : 0);
    }
#endif

    EpdFontLoader::loadFontsFromSd(renderer);
    Serial.printf("[%lu] [DBG] loadFontsFromSd done\n", millis());
#ifdef OMIT_FONTS
    Serial.printf("[%lu] [M4-FONT] SD custom fonts are optional. Format: /fonts/*.epdfont "
                  "(Fengyan EpdFontLoader V0/V1). .cpfont is NOT supported in this tree. "
                  "Builtin m4_ui_cjk covers UI strings; full-book CJK requires SD epdfont.\n",
                  millis());
#endif
#ifdef CROSSPOINT_MURPHY_M4
    // Header preflight + actual promotion result from loader (not header-only "loaded").
    {
      const bool exists = SdMan.exists(M4FontPolicy::kCanonicalSdPath);
      uint8_t hdr[48] = {};
      size_t fsize = 0;
      if (exists) {
        FsFile ff;
        if (SdMan.openFileForRead("M4Font", M4FontPolicy::kCanonicalSdPath, ff)) {
          fsize = static_cast<size_t>(ff.fileSize());
          ff.read(hdr, 48);
          ff.close();
        }
      }
      const M4FontPolicy::Preflight pf = M4FontPolicy::preflightFromHeader(hdr, fsize, exists);
      if (pf.status == M4FontPolicy::PreflightStatus::Missing) {
        Serial.printf("[%lu] [M4-FONT] PREFLIGHT missing path=%s (UI subset only)\n", millis(),
                      M4FontPolicy::kCanonicalSdPath);
      } else if (pf.status == M4FontPolicy::PreflightStatus::Invalid) {
        Serial.printf("[%lu] [M4-FONT] PREFLIGHT invalid size=%u %s\n", millis(),
                      static_cast<unsigned>(pf.fileSize), pf.diagnostic.c_str());
      } else {
        Serial.printf("[%lu] [M4-FONT] PREFLIGHT valid_header size=%u ver=%d %s sha_expect=%s\n", millis(),
                      static_cast<unsigned>(pf.fileSize), pf.headerVersion, pf.diagnostic.c_str(),
                      M4FontPolicy::kCanonicalArtifactSha256);
      }
      const auto lr = EpdFontLoader::lastCanonicalLoadResult();
      Serial.printf("[%lu] [M4-FONT] LOAD_RESULT %s\n", millis(), M4FontPolicy::loadResultName(lr));
      m4FontOk = M4FontPolicy::bootSummaryFontOk(lr);
      if (pf.status == M4FontPolicy::PreflightStatus::Invalid) m4FontOk = false;
    }
    printM4BootSummary(m4PsramOk, m4DispOk, m4SdOk, gpio.hasTouch(), m4TouchOk, m4FontOk, m4LightOk);
#endif
    Serial.flush();

#ifdef CROSSPOINT_MURPHY_M4
    {
      M4SerialDebug::HostHooks hooks;
      hooks.status = []() {
        M4SerialDebug::StatusSnapshot st;
        st.firmwareVersion = CROSSPOINT_VERSION;
        st.activity = currentActivity ? currentActivity->getName().c_str() : "";
        st.activeAppId = gDebugActiveAppId.c_str();
        st.freeHeap = ESP.getFreeHeap();
        st.minFreeHeap = ESP.getMinFreeHeap();
        st.freePsram = ESP.getFreePsram();
        st.resetReason = static_cast<uint32_t>(esp_reset_reason());
        st.sdOk = SdMan.ready();
        st.screenW = renderer.getScreenWidth();
        st.screenH = renderer.getScreenHeight();
        st.orientation = static_cast<int>(renderer.getOrientation());
        return st;
      };
      // Text-level UI dump for automated debug (prefer over OCR/screenshot).
      hooks.uiDump = []() -> std::string {
        if (!currentActivity) {
          return "{\"kind\":\"none\"}";
        }
        std::string body = currentActivity->debugUiJson();
        if (body.empty()) body = "{}";
        std::string out = "{\"activity_name\":\"";
        out += currentActivity->getName();
        out += "\",\"body\":";
        out += body;
        out += '}';
        return out;
      };
      hooks.goHome = []() { onGoHome(); };
      hooks.openFileTransferUi = []() { onGoToFileTransferUsb(); };
      hooks.noteActiveApp = [](const std::string& id) { gDebugActiveAppId = id; };
      hooks.clearActiveApp = []() { gDebugActiveAppId.clear(); };
      hooks.launchApp = [](const std::string& appId, std::string& errKey, std::string& errMsg) -> bool {
        if (!M4xIsValidPackageId(appId)) {
          errKey = "invalid_id";
          errMsg = "应用 ID 非法";
          return false;
        }
        const auto apps = M4xRegistry::load();
        const auto* app = M4xRegistry::find(apps, appId);
        if (!app) {
          errKey = "not_found";
          errMsg = "应用未安装";
          return false;
        }
        // Copy app record: registry vector is temporary.
        const M4xInstalledApp launched = *app;
        exitActivity();
        if (launched.runtime == M4xRuntimeKind::Native) {
          enterNewActivity(new NativeAppActivity(renderer, mappedInputManager, launched, []() {
            gDebugActiveAppId.clear();
            onGoHome();
          }));
        } else {
          enterNewActivity(new AppRuntimeActivity(renderer, mappedInputManager, launched, []() {
            // Return to safe home; avoid dangling activity pointers.
            gDebugActiveAppId.clear();
            onGoHome();
          }));
        }
        gDebugActiveAppId = appId;
        return true;
      };
      // Install runs synchronously on this main owner loop (no FreeRTOS worker).
      hooks.installSync = [](const std::string& inboxPath, std::string& errKey, std::string& errMsg, std::string& id,
                             std::string& ver, int& code) -> bool {
        const M4xInstallResult r = M4xInstaller::install(inboxPath);
        if (!r.ok) {
          errKey = r.error.empty() ? "install_fail" : r.error;
          errMsg = r.message.empty() ? "安装失败" : r.message;
          return false;
        }
        id = r.manifest.id;
        ver = r.manifest.version;
        code = r.manifest.versionCode;
        return true;
      };
      gM4DebugBridge.begin(&renderer, &mappedInputManager, &display, std::move(hooks));
      // Apply persisted setting (default off). No serial path can enable this.
      gM4DebugBridge.setAuthorized(SETTINGS.developerSerialDebugEnabled == 1);
    }
#endif

    exitActivity();
    enterNewActivity(new BootActivity(renderer, mappedInputManager));

    APP_STATE.loadFromFile();
    RECENT_BOOKS.loadFromFile();

    // Boot to home screen or reader
    if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
        mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
        Serial.printf("[%lu] [MAIN] home1\n", millis());
        onGoHome();
    } else {
        const auto path = APP_STATE.openEpubPath;
        // 查找最近阅读记录，获取原始源文件路径（如果是从 TXT 转换的 EPUB）
        std::string originalSourcePath;
        const auto& recentBooks = RECENT_BOOKS.getBooks();
        for (const auto& book : recentBooks) {
            if (book.path == path) {
                originalSourcePath = book.originalSourcePath;
                break;
            }
        }
        APP_STATE.openEpubPath = "";
        APP_STATE.readerActivityLoadCount++;
        APP_STATE.saveToFile();
        Serial.printf("[%lu] [MAIN] reader\n", millis());
        onGoToReader(path, originalSourcePath);
    }

#ifndef CROSSPOINT_X3
    // ========== X4: NTP时间同步已移至进入阅读器时执行 ==========
    // 每次开机只同步一次，由 APP_STATE.ntpSyncedThisBoot 控制
    // 在 EpubReaderActivity::onEnter() 中调用 syncNtpTime()
#endif  // CROSSPOINT_X3

    // Ensure we're not still holding the power button before leaving setup
    waitForPowerRelease();
}
void loop() {
#ifdef M4_QEMU_BUILD
  if (gM4QemuScreenMode) {
    // QEMU GPIO inputs float and can look like a held power key. Keep the first
    // rendered frame stable until explicit input modeling is added.
    delay(10);
    return;
  }
#endif
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

#ifdef CROSSPOINT_MURPHY_M4
  // A gesture transition is one-shot and is consumed by the next displayBuffer.
  // If a page rejects the gesture, the stale source expires after a few loops.
  renderer.ageEntryAnimation();
#endif

  gpio.update();

#ifndef CROSSPOINT_X3
  // NTP同步状态机（在主线程中执行，避免WiFi驱动问题）
  static unsigned long ntpStartTime = 0;
  static int ntpCredentialIndex = 0;
  static int ntpRetry = 0;
  static unsigned long lastNtpStateChange = 0;
  
  switch (ntpSyncState.load(std::memory_order_acquire)) {
    case NtpSyncState::PENDING: {
      // 初始化lastNtpStateChange
      if (lastNtpStateChange == 0) {
        lastNtpStateChange = millis();
      }
      
      // 延迟3秒后再开始，等待阅读器初始加载完成
      if (millis() - lastNtpStateChange < 3000) {
        break;  // 还没到3秒，等待
      }
      
      // 检查内存
      const uint32_t freeHeap = ESP.getFreeHeap();
      if (freeHeap < 100000) {  // 提高到100KB
        Serial.printf("[%lu] [NTP] Insufficient memory (%lu bytes), aborting\n", millis(), freeHeap);
        ntpSyncState.store(NtpSyncState::IDLE, std::memory_order_release);
        break;
      }
      
      // 加载WiFi凭据
      WIFI_STORE.loadFromFile();
      const auto& credentials = WIFI_STORE.getCredentials();
      
      if (credentials.empty()) {
        Serial.printf("[%lu] [NTP] No WiFi credentials, aborting\n", millis());
        ntpSyncState.store(NtpSyncState::IDLE, std::memory_order_release);
        break;
      }
      
      Serial.printf("[%lu] [NTP] Starting NTP sync in main thread\n", millis());
      WiFi.mode(WIFI_STA);
      ntpCredentialIndex = 0;
      ntpRetry = 0;
      ntpStartTime = millis();
      lastNtpStateChange = millis();
      ntpSyncState.store(NtpSyncState::CONNECTING, std::memory_order_release);
      break;
    }
    
    case NtpSyncState::CONNECTING: {
      const auto& credentials = WIFI_STORE.getCredentials();
      
      // 如果正在连接，检查状态
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[%lu] [NTP] WiFi connected: %s\n", millis(), WiFi.localIP().toString().c_str());
        configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
        ntpRetry = 0;
        ntpStartTime = millis();
        ntpSyncState.store(NtpSyncState::SYNCING, std::memory_order_release);
        break;
      }
      
      // 检查是否超时（8秒）
      if (millis() - ntpStartTime > 8000) {
        // 尝试下一个凭据
        ntpCredentialIndex++;
        if (ntpCredentialIndex >= std::min((int)credentials.size(), 3)) {
          Serial.printf("[%lu] [NTP] All WiFi credentials failed\n", millis());
          ntpSyncState.store(NtpSyncState::CLEANING_UP, std::memory_order_release);
          break;
        }
        
        // 连接下一个WiFi
        Serial.printf("[%lu] [NTP] Trying WiFi [%d]: %s\n", millis(), ntpCredentialIndex + 1, 
                      credentials[ntpCredentialIndex].ssid.c_str());
        WiFi.disconnect(true);
        delay(500);
        WiFi.begin(credentials[ntpCredentialIndex].ssid.c_str(), credentials[ntpCredentialIndex].password.c_str());
        ntpStartTime = millis();
        break;
      }
      break;
    }
    
    case NtpSyncState::SYNCING: {
      // 检查NTP是否同步完成
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        struct tm timeinfo;
        time_t now = time(nullptr);
        localtime_r(&now, &timeinfo);
        Serial.printf("[%lu] [NTP] Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                      millis(), timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        esp_sntp_stop();
        ntpSyncState.store(NtpSyncState::CLEANING_UP, std::memory_order_release);
        break;
      }
      
      // 检查是否超时（5秒）
      if (millis() - ntpStartTime > 5000) {
        Serial.printf("[%lu] [NTP] NTP sync timeout\n", millis());
        esp_sntp_stop();
        ntpSyncState.store(NtpSyncState::CLEANING_UP, std::memory_order_release);
        break;
      }
      break;
    }
    
    case NtpSyncState::CLEANING_UP: {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      esp_wifi_deinit();
      Serial.printf("[%lu] [NTP] WiFi cleanup complete\n", millis());
      ntpSyncState.store(NtpSyncState::IDLE, std::memory_order_release);
      ntpSyncDone.store(true, std::memory_order_release);  // 通知其他模块
      break;
    }
    
    case NtpSyncState::IDLE:
    default:
      break;
  }
#endif

#ifdef CROSSPOINT_X3
  {
    // tiltScope: 0=仅阅读生效, 1=全局生效
    bool inReader = currentActivity && currentActivity->isReaderActivity();
    bool shouldRun = (SETTINGS.tiltScope == 1) || inReader;
    tiltDetector.setSuspended(!shouldRun);
    tiltDetector.update();
  }
#endif  // CROSSPOINT_X3

    // Check for Bluetooth inactivity timeouts and auto-reconnect
  try {
    BluetoothHIDManager::getInstance().updateActivity();
    BluetoothHIDManager::getInstance().checkAutoReconnect();
    
    // 蓝牙断开通知：在主线程中显示提示
    if (BluetoothHIDManager::getInstance().consumeDisconnectNotify()) {
      GUI.drawPopup(renderer, L(Str::kBluetoothDisconnected));
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  } catch (...) {
    // Ignore errors in Bluetooth management
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    Serial.printf("[%lu] [MEM] Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", millis(), ESP.getFreeHeap(),
                  ESP.getHeapSize(), ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  // Check for physical button presses, virtual button presses, or activity prevention
  bool hasActivity = gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() ||
                     (currentActivity && currentActivity->preventAutoSleep());
#ifdef CROSSPOINT_MURPHY_M4
  // Keep awake only while authorized AND host is actively scripting (bounded window).
  hasActivity = hasActivity || gM4DebugBridge.recentHostActivity(millis());
#endif
  
  // Also check for recent BLE activity to prevent power sleep during BLE use
  try {
    const auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) {
      // If BLE is enabled, check if there's been recent activity
      // We consider that activity if the manager has been tracking it
      // (This prevents the system from sleeping while using BLE controller)
      hasActivity = hasActivity || btMgr.hasRecentActivity();
    }
  } catch (...) {
    // Ignore BLE check errors
  }
  
  if (hasActivity) {
    lastActivityTime = millis();  // Reset inactivity timer
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    Serial.printf("[%lu] [SLP] Auto-sleep triggered after %lu ms of inactivity\n", millis(), sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // 短按电源键功能处理
  if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
    // 检查短按电源键的设置
    if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FULL_REFRESH) {
      // 全刷功能：在阅读器中跳过（阅读器有自己的刷新管理，避免双重全刷）
      if (currentActivity && currentActivity->isReaderActivity()) {
        Serial.printf("[%lu] [PWR] Full refresh skipped (reader manages its own refresh)\n", millis());
      } else {
        Serial.printf("[%lu] [PWR] Full refresh triggered by power button\n", millis());
        renderer.displayBuffer(HalDisplay::FULL_REFRESH);
      }
    } else if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::CONFIRM) {
      // 确认功能：模拟确认键按下和释放
      // 通过注入按钮事件实现
      gpio.injectButtonPress(HalGPIO::BTN_CONFIRM);
    }
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Long press Back button (1.5s) → go home from any non-home page
  static bool longPressBackHomeFired = false;
  if (currentActivity && !currentActivity->isHomeActivity() &&
      mappedInputManager.isPressed(MappedInputManager::Button::Back) &&
      mappedInputManager.getHeldTime() >= 1500) {
    if (!longPressBackHomeFired) {
      longPressBackHomeFired = true;
      onGoHome();
      return;
    }
  } else if (!mappedInputManager.isPressed(MappedInputManager::Button::Back)) {
    longPressBackHomeFired = false;
  }

  // Global full-screen navigation gestures (all activities):
  //   • edge swipe (either direction) → Back (synthetic button so every page responds)
  //   • bottom-edge swipe up  → Home
  mappedInputManager.beginFrame();
#ifdef CROSSPOINT_MURPHY_M4
  if (mappedInputManager.hasTouch() && currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInputManager.wasHomeGesture()) {
      Serial.printf("[%lu] [M4-GESTURE] home (bottom swipe up)\n", millis());
      // Only the bottom-edge swipe animates. The on-screen Home control is
      // intentionally immediate, like the physical button.
      const bool swipe = mappedInputManager.wasHomeSwipeGesture();
      onGoHomeAnimated(swipe, /*logical bottom→top=*/2);
      // Home activity entered; never run the old activity again this frame.
      return;
    } else if (!currentActivity->isHomeActivity() && mappedInputManager.wasBackGesture()) {
      Serial.printf("[%lu] [M4-GESTURE] back (edge swipe)\n", millis());
      // Arm only for the touch gesture. The destination may be a new activity
      // or a child activity closing inside the current one; both paths render
      // through GfxRenderer::displayBuffer(). Physical Back stays immediate.
      // Only the edge swipe animates. The on-screen Back control and physical
      // Back button remain immediate.
      const int logicalBackDirection = mappedInputManager.backSwipeAnimationDirection();
      if (logicalBackDirection >= 0) {
        renderer.armEntryAnimation(logicalBackDirection);
      }
      // Pulse logical Back so button paths and wasBackGesture both see it.
      mappedInputManager.pulseSyntheticBack();
    }
  }
#endif

#ifdef CROSSPOINT_MURPHY_M4
  // Apply Developer Options switch every frame (idempotent). Local UI only.
  gM4DebugBridge.setAuthorized(SETTINGS.developerSerialDebugEnabled == 1);
  // After beginFrame (clears prior synthetic), before activity loop consumes input.
  gM4DebugBridge.poll();
#endif

  const unsigned long activityStartTime = millis();
  if (currentActivity) {
    currentActivity->loop();
  }
  const unsigned long activityDuration = millis() - activityStartTime;

#ifdef CROSSPOINT_MURPHY_M4
  // Re-apply if settings UI changed brightness/warmth.
  applyFrontlightSettings(false);
#endif

  // Process deferred activity deletion after loop() returns.
  // At this point we're safely outside any activity's call stack.
  if (deferredDeleteActivity) {
    delete deferredDeleteActivity;
    deferredDeleteActivity = nullptr;
  }

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      Serial.printf("[%lu] [LOOP] New max loop duration: %lu ms (activity: %lu ms)\n", millis(), maxLoopDuration,
                    activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();  // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    delay(10);  // Normal delay when no activity requires fast response
  }
}

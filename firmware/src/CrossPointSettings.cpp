#include "CrossPointSettings.h"

#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <climits>
#include <cstdio>
#include <cstring>

#include "fontIds.h"
#include "managers/FontManager.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
// JSON format (primary storage)
constexpr char SETTINGS_JSON_FILE[] = "/.crosspoint/settings.json";
constexpr uint8_t SETTINGS_JSON_VERSION = 1;
// Binary format (legacy – kept for one-time migration only)
constexpr uint8_t SETTINGS_FILE_VERSION = 5;
constexpr char SETTINGS_FILE[] = "/.crosspoint/settings.bin";

// Validate front button mapping to ensure each hardware button is unique.
// If duplicates are detected, reset to the default physical order to prevent invalid mappings.
void validateFrontButtonMapping(CrossPointSettings& settings) {
  // Snapshot the logical->hardware mapping so we can compare for duplicates.
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        // Duplicate detected: restore the default physical order (Back, Confirm, Left, Right).
        settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
        settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
        settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
        settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}
}  // namespace

bool CrossPointSettings::saveToFile() const {
  SdMan.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", SETTINGS_JSON_FILE, outputFile)) {
    return false;
  }

  JsonDocument doc;
  doc["v"]                         = SETTINGS_JSON_VERSION;
  // Sleep screen
  doc["sleepScreen"]               = sleepScreen;
  doc["sleepScreenCoverMode"]      = sleepScreenCoverMode;
  doc["sleepScreenCoverFilter"]    = sleepScreenCoverFilter;
  doc["transparentOverlayPxc"]     = transparentOverlayPxc;
  doc["sleepPngInvert"]            = sleepPngInvert;
  doc["sleepBeforeFullRefresh"]    = sleepBeforeFullRefresh;
  // Page-turn animation
  doc["pageTurnAnimationEnabled"]    = pageTurnAnimationEnabled;
  doc["pageTurnAnimationSteps"]      = pageTurnAnimationSteps;
  doc["pageTurnAnimationMult"]       = pageTurnAnimationMult;
  doc["pageTurnAnimationTp"]         = pageTurnAnimationTp;
  doc["pageTurnAnimationFrameRate"]  = pageTurnAnimationFrameRate;
  doc["pageTurnAnimationDir"]        = pageTurnAnimationDir;
  // Power & system
  doc["shortPwrBtn"]               = shortPwrBtn;
  doc["sleepTimeout"]              = sleepTimeout;
  doc["longPressBoot"]             = longPressBoot;
  doc["autoSyncTimeOnBoot"]        = autoSyncTimeOnBoot;
  // Display & UI
  doc["statusBar"]                 = statusBar;
  doc["orientation"]               = orientation;
  doc["uiTheme"]                   = uiTheme;
  doc["homeIconStyle"]             = homeIconStyle;
  doc["iconStyle"]                 = iconStyle;
  doc["hideBatteryPercentage"]     = hideBatteryPercentage;
  doc["buttonHintsEnabled"]        = buttonHintsEnabled;
  doc["frontlightBrightness"]      = frontlightBrightness;
  doc["frontlightWarmth"]          = frontlightWarmth;
  doc["showTimeInsteadOfChapter"]  = showTimeInsteadOfChapter;
  doc["ReadingScreenEnabled"]      = ReadingScreenEnabled;
  // Refresh & rendering
  doc["refreshFrequency"]          = refreshFrequency;
  doc["neverFullRefresh"]          = neverFullRefresh;
  doc["imageQuality"]              = imageQuality;
  doc["fadingFix"]                 = fadingFix;
  // Font & typography
  doc["fontFamily"]                = fontFamily;
  doc["fontSize"]                  = fontSize;
  doc["customFontFamily"]          = customFontFamily;
  doc["customFontSize"]            = customFontSize;
  doc["lineSpacing"]               = lineSpacing;
  doc["customLineSpacing"]         = customLineSpacing;
  doc["wordSpacing"]               = (int)wordSpacing;
  doc["firstlineintented"]         = firstlineintented;
  doc["underlineOffset"]           = (int)underlineOffset;
  doc["underlineStyle"]            = underlineStyle;
  doc["paragraphAlignment"]        = paragraphAlignment;
  doc["chinesePunctWidth"]         = chinesePunctWidth;
  doc["extraParagraphSpacing"]     = extraParagraphSpacing;
  doc["textAntiAliasing"]          = textAntiAliasing;
  doc["hyphenationEnabled"]        = hyphenationEnabled;
  doc["embeddedStyle"]             = embeddedStyle;
  doc["epubShowImages"]            = epubShowImages;
  doc["epubDarkMode"]              = epubDarkMode;
  doc["extraline"]                 = extraline;
  // Screen margins
  doc["screenMarginTop"]           = screenMargin_Top;
  doc["screenMarginBottom"]        = screenMargin_Bottom;
  doc["screenMarginLeft"]          = screenMargin_Left;
  doc["screenMarginRight"]         = screenMargin_Right;
  // Button layout & actions
  doc["frontButtonLayout"]         = frontButtonLayout;
  doc["sideButtonLayout"]          = sideButtonLayout;
  doc["frontButtonBack"]           = frontButtonBack;
  doc["frontButtonConfirm"]        = frontButtonConfirm;
  doc["frontButtonLeft"]           = frontButtonLeft;
  doc["frontButtonRight"]          = frontButtonRight;
  doc["longPressChapterSkip"]      = longPressChapterSkip;
  doc["longPressConfirmAction"]    = longPressConfirmAction;
  doc["libraryLongPressMenu"]      = libraryLongPressMenu;
  // Global feature switches
  doc["globalNextPageModeEnabled"] = globalNextPageModeEnabled;
  doc["wifiAlwaysReselect"]        = wifiAlwaysReselect;
  doc["directTxtRead"]             = directTxtRead;
  doc["systemLanguage"]            = systemLanguage;
  doc["developerSerialDebugEnabled"] = developerSerialDebugEnabled;
  doc["libraryHomePath"]           = libraryHomePath;
  // Auto page turn
  doc["autoPageTurnEnabled"]       = autoPageTurnEnabled;
  doc["autoPageTurnMode"]          = autoPageTurnMode;
  doc["autoPageTurnInterval"]      = autoPageTurnInterval;
  // Tilt page turn
  doc["tiltPageTurnEnabled"]       = tiltPageTurnEnabled;
  doc["tiltScope"]                 = tiltScope;
  doc["tiltTriggerAngle"]          = tiltTriggerAngle;
  doc["tiltReleaseAngle"]          = tiltReleaseAngle;
  doc["tiltHoldTimeMs"]            = tiltHoldTimeMs;
  doc["tiltCooldownTimeMs"]        = tiltCooldownTimeMs;
  doc["tiltLeftAction"]            = tiltLeftAction;
  doc["tiltRightAction"]           = tiltRightAction;
  doc["tiltLargeLeftAction"]       = tiltLargeLeftAction;
  doc["tiltLargeRightAction"]      = tiltLargeRightAction;
  // Tap page turn
  doc["tapPageTurnEnabled"]        = tapPageTurnEnabled;
  doc["tapAction"]                 = tapAction;
  doc["tapThresholdG"]             = tapThresholdG;
  doc["tapCooldownMs"]             = tapCooldownMs;
  // Auto-rotate
  doc["autoRotateEnabled"]         = autoRotateEnabled;
  // Landscape dual-page
  doc["landscapeDualPageEnabled"]  = landscapeDualPageEnabled;
  // Misc
  doc["transparentRemoveWhite"]     = transparentRemoveWhite;
  // Bluetooth
  doc["bluetoothEnabled"]          = bluetoothEnabled;
  doc["btKey1Code"]                = btKey1Code;
  doc["btKey1Action"]              = btKey1Action;
  doc["btKey2Code"]                = btKey2Code;
  doc["btKey2Action"]              = btKey2Action;
  doc["btKey3Code"]                = btKey3Code;
  doc["btKey3Action"]              = btKey3Action;
  doc["btKey4Code"]                = btKey4Code;
  doc["btKey4Action"]              = btKey4Action;
  doc["btKey5Code"]                = btKey5Code;
  doc["btKey5Action"]              = btKey5Action;
  doc["btKey6Code"]                = btKey6Code;
  doc["btKey6Action"]              = btKey6Action;
  // Accounts
  doc["opdsServerUrl"]             = opdsServerUrl;
  doc["opdsUsername"]              = opdsUsername;
  doc["opdsPassword"]              = opdsPassword;
  doc["jgUsername"]                = jgUsername;
  doc["jgAppPassword"]             = jgAppPassword;
  doc["jgBookFolder"]              = jgBookFolder;
  doc["dcUsername"]                = dcUsername;
  doc["dcPassword"]                = dcPassword;
  doc["dcWebdavUrl"]               = dcWebdavUrl;
  doc["zlibEmail"]                 = zlibEmail;
  doc["zlibPassword"]              = zlibPassword;

  serializeJson(doc, outputFile);
  outputFile.close();
  Serial.printf("[%lu] [CPS] Settings saved to JSON\n", millis());
  return true;
}

void CrossPointSettings::resetToDefaults() {
  // 重置所有显示/阅读/控制/系统相关设置为出厂默认值
  // 注意：账号密码类字段（OPDS/坚果云/数据胶囊/ZLib）不重置
  sleepScreen = LIGHT;
  sleepScreenCoverMode = FIT;
  sleepScreenCoverFilter = NO_FILTER;
  statusBar = FULL;
  extraParagraphSpacing = 0;
  textAntiAliasing = 0;
  shortPwrBtn = PAGE_TURN;
  orientation = PORTRAIT;
  frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  sideButtonLayout = PREV_NEXT;
  frontButtonBack = FRONT_HW_BACK;
  frontButtonConfirm = FRONT_HW_CONFIRM;
  frontButtonLeft = FRONT_HW_LEFT;
  frontButtonRight = FRONT_HW_RIGHT;
  fontFamily = SYSTEM_FONT;
  customFontSize = 0;
  customFontFamily[0] = '\0';
  fontSize = LARGE;
  lineSpacing = NORMAL;
  customLineSpacing = 10;
  firstlineintented = 1;
  wordSpacing = 0;
  underlineOffset = 5;
  underlineStyle = 1;
  paragraphAlignment = JUSTIFIED;
  chinesePunctWidth = PUNCT_COMPACT;
  epubShowImages = 1;
  sleepTimeout = SLEEP_10_MIN;
  refreshFrequency = 10;
  hyphenationEnabled = 0;
  screenMargin_Top = 10;
  screenMargin_Left = 10;
  screenMargin_Bottom = 10;
  screenMargin_Right = 10;
  extraline = 1;
  bluetoothEnabled = 0;
  ReadingScreenEnabled = 0;
  hideBatteryPercentage = HIDE_ALWAYS;
  longPressBoot = 1;
  neverFullRefresh = 0;
  longPressChapterSkip = 1;
  globalNextPageModeEnabled = 0;
  uiTheme = LYRA;
  fadingFix = 0;
  embeddedStyle = 0;
  libraryHomePath[0] = '\0';
  autoPageTurnEnabled = 0;
  autoPageTurnMode = 1;
  autoPageTurnInterval = 10;
  tiltPageTurnEnabled = 0;
  tiltScope = 0;
  tiltTriggerAngle = 20;
  tiltReleaseAngle = 10;
  tiltHoldTimeMs = 200;
  tiltCooldownTimeMs = 800;
  tiltLeftAction = 0;
  tiltRightAction = 1;
  tiltLargeLeftAction = 0;
  tiltLargeRightAction = 1;
  tapPageTurnEnabled = 0;
  tapAction = 1;
  tapThresholdG = 12;
  tapCooldownMs = 600;
  autoRotateEnabled = 0;
  landscapeDualPageEnabled = 0;
  buttonHintsEnabled = 0;
  wifiAlwaysReselect = 1;
  sleepPngInvert = 1;
  sleepBeforeFullRefresh = 1;
  imageQuality = QUALITY_NORMAL;
  transparentOverlayPxc[0] = '\0';
  epubDarkMode = 0;
  homeIconStyle = CORNERS_ONLY;
  iconStyle = 0;
  transparentRemoveWhite = 1;
#ifdef CROSSPOINT_X3
  longPressConfirmAction = 6;  // 默认无
#else
  longPressConfirmAction = 5;  // 默认无
#endif
  libraryLongPressMenu = 0;
  btKey1Code = 0x00; btKey1Action = 0xFF;
  btKey2Code = 0x00; btKey2Action = 0xFF;
  btKey3Code = 0x00; btKey3Action = 0xFF;
  btKey4Code = 0x00; btKey4Action = 0xFF;
  btKey5Code = 0x00; btKey5Action = 0xFF;
  btKey6Code = 0x00; btKey6Action = 0xFF;
  autoSyncTimeOnBoot = 0;
  systemLanguage = 0;  // 简体中文
  developerSerialDebugEnabled = 0;
  Serial.printf("[%lu] [CPS] Settings reset to defaults\n", millis());
}

bool CrossPointSettings::loadFromBinaryFile() {
  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  // load settings that exist (support older files with fewer fields)
  uint8_t settingsRead = 0;
  // Track whether remap fields were present in the settings file.
  bool frontButtonMappingRead = false;
  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontFamily, FONT_FAMILY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, customLineSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, wordSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, underlineOffset);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, underlineStyle, 5);  // 0-4: 5 underline styles
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, firstlineintented);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    // Read refreshFrequency and migrate from old enum format if needed
    serialization::readPod(inputFile, refreshFrequency);
    if (refreshFrequency < 5) {
      // Old format: enum index (0-4) -> convert to actual page count
      const uint8_t oldToNew[] = {1, 5, 10, 15, 30};
      refreshFrequency = oldToNew[refreshFrequency];
    }
    // Clamp to valid range
    if (refreshFrequency < 1) refreshFrequency = 1;
    if (refreshFrequency > 30) refreshFrequency = 30;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin_Top);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin_Bottom);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin_Left);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin_Right);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, longPressChapterSkip);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string fontStr;
      serialization::readString(inputFile, fontStr);
      strncpy(customFontFamily, fontStr.c_str(), sizeof(customFontFamily) - 1);
      customFontFamily[sizeof(customFontFamily) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, customFontSize);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(jgBookFolder, urlStr.c_str(), sizeof(jgBookFolder) - 1);
      jgBookFolder[sizeof(jgBookFolder) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(jgUsername, usernameStr.c_str(), sizeof(jgUsername) - 1);
      jgUsername[sizeof(jgUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(jgAppPassword, passwordStr.c_str(), sizeof(jgAppPassword) - 1);
      jgAppPassword[sizeof(jgAppPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    
    // 修复点2：读取sleepScreenCoverFilter（和写入顺序对应）
    serialization::readPod(inputFile, sleepScreenCoverFilter);
    if (++settingsRead >= fileSettingsCount) break;
    
    // 修复点3：uiTheme读取位置修正（原代码位置错误导致后续字段错位）
    serialization::readPod(inputFile, uiTheme);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
    //新加阅读背景
    serialization::readPod(inputFile, ReadingScreenEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    //新加划线功能
    serialization::readPod(inputFile, extraline);
    if (++settingsRead >= fileSettingsCount) break;
        //新加蓝牙功能
    serialization::readPod(inputFile, bluetoothEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, globalNextPageModeEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string homeStr;
      serialization::readString(inputFile, homeStr);
      strncpy(libraryHomePath, homeStr.c_str(), sizeof(libraryHomePath) - 1);
      libraryHomePath[sizeof(libraryHomePath) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    // Auto page turn settings
    serialization::readPod(inputFile, autoPageTurnEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, autoPageTurnMode);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, autoPageTurnInterval);
    if (autoPageTurnInterval < 1) autoPageTurnInterval = 1;
    if (autoPageTurnInterval > 60) autoPageTurnInterval = 60;
    if (++settingsRead >= fileSettingsCount) break;
    // 数据胶囊 settings
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(dcUsername, usernameStr.c_str(), sizeof(dcUsername) - 1);
      dcUsername[sizeof(dcUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(dcPassword, passwordStr.c_str(), sizeof(dcPassword) - 1);
      dcPassword[sizeof(dcPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    // Tilt page turn settings (晃动翻页)
    serialization::readPod(inputFile, tiltPageTurnEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltTriggerAngle);
    if (tiltTriggerAngle < 1) tiltTriggerAngle = 1;
    if (tiltTriggerAngle > 90) tiltTriggerAngle = 90;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltReleaseAngle);
    if (tiltReleaseAngle > 89) tiltReleaseAngle = 89;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltHoldTimeMs);
    if (tiltHoldTimeMs < 50) tiltHoldTimeMs = 50;
    if (tiltHoldTimeMs > 2000) tiltHoldTimeMs = 2000;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltCooldownTimeMs);
    if (tiltCooldownTimeMs < 100) tiltCooldownTimeMs = 100;
    if (tiltCooldownTimeMs > 5000) tiltCooldownTimeMs = 5000;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltLeftAction);
    if (tiltLeftAction > 1) tiltLeftAction = 0;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltRightAction);
    if (tiltRightAction > 1) tiltRightAction = 1;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltLargeLeftAction);
    if (tiltLargeLeftAction > 3) tiltLargeLeftAction = 0;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltLargeRightAction);
    if (tiltLargeRightAction > 3) tiltLargeRightAction = 1;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tiltScope);
    if (tiltScope > 1) tiltScope = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // Button hints
    serialization::readPod(inputFile, buttonHintsEnabled);
    if (buttonHintsEnabled > 1) buttonHintsEnabled = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // PNG/JPG 壁纸反转模式
    serialization::readPod(inputFile, sleepPngInvert);
    if (sleepPngInvert > 1) sleepPngInvert = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // 关机前全刷
    serialization::readPod(inputFile, sleepBeforeFullRefresh);
    if (sleepBeforeFullRefresh > 1) sleepBeforeFullRefresh = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // 图片渲染质量
    serialization::readPod(inputFile, imageQuality);
    if (imageQuality > 2) imageQuality = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // 透明叠加壁纸 .pxc 路径（128 字节固定长度，算作 1 个字段）
    {
      char tmp[128] = "";
      if (inputFile.read(tmp, sizeof(tmp)) == sizeof(tmp)) {
        tmp[sizeof(tmp) - 1] = '\0';
        memcpy(transparentOverlayPxc, tmp, sizeof(tmp));
      }
    }
    if (++settingsRead >= fileSettingsCount) break;
    // EPUB 阅读暗黑模式
    serialization::readPod(inputFile, epubDarkMode);
    if (epubDarkMode > 1) epubDarkMode = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // 长按开机
    serialization::readPod(inputFile, longPressBoot);
    if (longPressBoot > 1) longPressBoot = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // 永不全刷
    serialization::readPod(inputFile, neverFullRefresh);
    if (neverFullRefresh > 1) neverFullRefresh = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // 主页面图标风格
    serialization::readPod(inputFile, homeIconStyle);
    if (homeIconStyle >= HOME_ICON_STYLE_COUNT) homeIconStyle = CORNERS_AND_GRAY_BG;
    if (++settingsRead >= fileSettingsCount) break;
    // 透明壁纸去白色
    serialization::readPod(inputFile, transparentRemoveWhite);
    if (transparentRemoveWhite > 1) transparentRemoveWhite = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // 图标风格
    serialization::readPod(inputFile, iconStyle);
    if (iconStyle > 2) iconStyle = 0;  // 0=风格一, 1=风格二, 2=风格三
    if (++settingsRead >= fileSettingsCount) break;
    // 长按确认键功能映射
    serialization::readPod(inputFile, longPressConfirmAction);
#ifdef CROSSPOINT_X3
    if (longPressConfirmAction > 6) longPressConfirmAction = 6;  // 默认无
#else
    if (longPressConfirmAction > 5) longPressConfirmAction = 5;  // 默认无
#endif
    if (++settingsRead >= fileSettingsCount) break;
    // 文件管理器确认键行为
    serialization::readPod(inputFile, libraryLongPressMenu);
    if (libraryLongPressMenu > 1) libraryLongPressMenu = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // 中文标点宽度
    serialization::readPod(inputFile, chinesePunctWidth);
    if (chinesePunctWidth > 1) chinesePunctWidth = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // 显示epub图片
    serialization::readPod(inputFile, epubShowImages);
    if (epubShowImages > 1) epubShowImages = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // 蓝牙按键自定义映射（6个槽）
    serialization::readPod(inputFile, btKey1Code);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey1Action);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey2Code);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey2Action);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey3Code);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey3Action);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey4Code);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey4Action);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey5Code);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey5Action);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey6Code);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, btKey6Action);
    if (++settingsRead >= fileSettingsCount) break;
    // 数据胶囊WebDAV地址
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(dcWebdavUrl, urlStr.c_str(), sizeof(dcWebdavUrl) - 1);
      dcWebdavUrl[sizeof(dcWebdavUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    // 开机自动同步时间
    serialization::readPod(inputFile, autoSyncTimeOnBoot);
    if (autoSyncTimeOnBoot > 1) autoSyncTimeOnBoot = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // 显示时间而非章节名
    serialization::readPod(inputFile, showTimeInsteadOfChapter);
    if (showTimeInsteadOfChapter > 1) showTimeInsteadOfChapter = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // 直读TXT文档
    serialization::readPod(inputFile, directTxtRead);
    if (directTxtRead > 1) directTxtRead = 1;
    if (++settingsRead >= fileSettingsCount) break;
    // Tap page turn settings (敲击翻页)
    serialization::readPod(inputFile, tapPageTurnEnabled);
    if (tapPageTurnEnabled > 1) tapPageTurnEnabled = 0;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tapAction);
    if (tapAction > 1) tapAction = 1;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tapThresholdG);
    if (tapThresholdG < 10) tapThresholdG = 10;
    if (tapThresholdG > 80) tapThresholdG = 80;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tapCooldownMs);
    if (tapCooldownMs < 200) tapCooldownMs = 200;
    if (tapCooldownMs > 3000) tapCooldownMs = 3000;
    if (++settingsRead >= fileSettingsCount) break;
    // Auto-rotate
    serialization::readPod(inputFile, autoRotateEnabled);
    if (autoRotateEnabled > 1) autoRotateEnabled = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // Landscape dual-page
    serialization::readPod(inputFile, landscapeDualPageEnabled);
    if (landscapeDualPageEnabled > 1) landscapeDualPageEnabled = 0;
    if (++settingsRead >= fileSettingsCount) break;
    // New fields added at end for backward compatibility
  } while (false);

  if (frontButtonMappingRead) {
    validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  inputFile.close();
  Serial.printf("[%lu] [CPS] Binary settings loaded (for migration)\n", millis());
  return true;
}

bool CrossPointSettings::loadFromFile() {
  // ── 1. 优先尝试 JSON 格式 ─────────────────────────────────────────
  if (SdMan.exists(SETTINGS_JSON_FILE)) {
    FsFile inputFile;
    if (SdMan.openFileForRead("CPS", SETTINGS_JSON_FILE, inputFile)) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, inputFile);
      inputFile.close();

      if (!err) {
        uint8_t v = doc["v"] | (uint8_t)0;
        if (v == SETTINGS_JSON_VERSION) {
          // 辅助函数：安全读取字符串字段
          auto getString = [&](const char* key, char* dst, size_t sz, const char* def = "") {
            const char* val = doc[key] | def;
            strncpy(dst, val, sz - 1);
            dst[sz - 1] = '\0';
          };

          // Sleep screen
          sleepScreen              = doc["sleepScreen"]              | (uint8_t)LIGHT;
          sleepScreenCoverMode     = doc["sleepScreenCoverMode"]     | (uint8_t)FIT;
          sleepScreenCoverFilter   = doc["sleepScreenCoverFilter"]   | (uint8_t)NO_FILTER;
          getString("transparentOverlayPxc", transparentOverlayPxc, sizeof(transparentOverlayPxc));
          sleepPngInvert           = doc["sleepPngInvert"]           | (uint8_t)1;
          sleepBeforeFullRefresh   = doc["sleepBeforeFullRefresh"]   | (uint8_t)1;
          // Page-turn animation (sanitize: FrameRate is a raw LUT byte, Dir is 0..3 index)
          pageTurnAnimationEnabled   = doc["pageTurnAnimationEnabled"]   | (uint8_t)0;
          if (pageTurnAnimationEnabled > 1) pageTurnAnimationEnabled = 0;
          pageTurnAnimationSteps     = doc["pageTurnAnimationSteps"]     | (uint8_t)9;
          if (pageTurnAnimationSteps < 2) pageTurnAnimationSteps = 2;
          if (pageTurnAnimationSteps > 64) pageTurnAnimationSteps = 64;
          pageTurnAnimationMult      = doc["pageTurnAnimationMult"]      | (uint8_t)4;
          if (pageTurnAnimationMult < 1) pageTurnAnimationMult = 1;
          if (pageTurnAnimationMult > 16) pageTurnAnimationMult = 16;
          pageTurnAnimationTp        = doc["pageTurnAnimationTp"]        | (uint8_t)0x02;
          if (pageTurnAnimationTp < 1) pageTurnAnimationTp = 1;
          if (pageTurnAnimationTp > 16) pageTurnAnimationTp = 16;
          pageTurnAnimationFrameRate = doc["pageTurnAnimationFrameRate"] | (uint8_t)0x88;
          if (pageTurnAnimationFrameRate != 0x22 && pageTurnAnimationFrameRate != 0x44 &&
              pageTurnAnimationFrameRate != 0x88) {
            pageTurnAnimationFrameRate = 0x88;
          }
          pageTurnAnimationDir       = doc["pageTurnAnimationDir"]       | (uint8_t)0;
          if (pageTurnAnimationDir > 3) pageTurnAnimationDir = 0;
          // Power & system
          shortPwrBtn              = doc["shortPwrBtn"]              | (uint8_t)PAGE_TURN;
          sleepTimeout             = doc["sleepTimeout"]             | (uint8_t)SLEEP_10_MIN;
          longPressBoot            = doc["longPressBoot"]            | (uint8_t)1;
          autoSyncTimeOnBoot       = doc["autoSyncTimeOnBoot"]       | (uint8_t)0;
          // Display & UI
          statusBar                = doc["statusBar"]                | (uint8_t)FULL;
          orientation              = doc["orientation"]              | (uint8_t)PORTRAIT;
          uiTheme                  = doc["uiTheme"]                  | (uint8_t)LYRA;
          homeIconStyle            = doc["homeIconStyle"]            | (uint8_t)CORNERS_ONLY;
          iconStyle                = doc["iconStyle"]                | (uint8_t)0;
          hideBatteryPercentage    = doc["hideBatteryPercentage"]    | (uint8_t)HIDE_ALWAYS;
          buttonHintsEnabled       = doc["buttonHintsEnabled"]       | (uint8_t)0;
          frontlightBrightness     = doc["frontlightBrightness"]     | (uint8_t)20;
          if (frontlightBrightness > 100) frontlightBrightness = 100;
          frontlightWarmth         = doc["frontlightWarmth"]         | (uint8_t)50;
          if (frontlightWarmth > 100) frontlightWarmth = 100;
          showTimeInsteadOfChapter = doc["showTimeInsteadOfChapter"] | (uint8_t)1;
          ReadingScreenEnabled     = doc["ReadingScreenEnabled"]     | (uint8_t)0;
          // Refresh & rendering
          refreshFrequency         = doc["refreshFrequency"]         | (uint8_t)10;
          if (refreshFrequency < 1) refreshFrequency = 1;
          if (refreshFrequency > 30) refreshFrequency = 30;
          neverFullRefresh         = doc["neverFullRefresh"]         | (uint8_t)0;
          imageQuality             = doc["imageQuality"]             | (uint8_t)QUALITY_NORMAL;
          fadingFix                = doc["fadingFix"]                | (uint8_t)0;
          // Font & typography
          fontFamily               = doc["fontFamily"]               | (uint8_t)SYSTEM_FONT;
          fontSize                 = doc["fontSize"]                 | (uint8_t)LARGE;
          getString("customFontFamily", customFontFamily, sizeof(customFontFamily));
          customFontSize           = doc["customFontSize"]           | (uint8_t)0;
          lineSpacing              = doc["lineSpacing"]              | (uint8_t)NORMAL;
          customLineSpacing        = doc["customLineSpacing"]        | (uint8_t)10;
          wordSpacing              = static_cast<int8_t>(doc["wordSpacing"] | 0);
          firstlineintented        = doc["firstlineintented"]        | (uint8_t)1;
          underlineOffset          = static_cast<int8_t>(doc["underlineOffset"] | 5);
          underlineStyle           = doc["underlineStyle"]           | (uint8_t)1;
          paragraphAlignment       = doc["paragraphAlignment"]       | (uint8_t)JUSTIFIED;
          chinesePunctWidth        = doc["chinesePunctWidth"]        | (uint8_t)PUNCT_COMPACT;
          extraParagraphSpacing    = doc["extraParagraphSpacing"]    | (uint8_t)0;
          textAntiAliasing         = doc["textAntiAliasing"]         | (uint8_t)0;
          hyphenationEnabled       = doc["hyphenationEnabled"]       | (uint8_t)0;
          embeddedStyle            = doc["embeddedStyle"]            | (uint8_t)0;
          epubShowImages           = doc["epubShowImages"]           | (uint8_t)1;
          epubDarkMode             = doc["epubDarkMode"]             | (uint8_t)0;
          extraline                = doc["extraline"]                | (uint8_t)1;
          // Screen margins
          screenMargin_Top         = doc["screenMarginTop"]          | (uint8_t)10;
          screenMargin_Bottom      = doc["screenMarginBottom"]       | (uint8_t)10;
          screenMargin_Left        = doc["screenMarginLeft"]         | (uint8_t)10;
          screenMargin_Right       = doc["screenMarginRight"]        | (uint8_t)10;
          // Button layout & actions
          frontButtonLayout        = doc["frontButtonLayout"]        | (uint8_t)BACK_CONFIRM_LEFT_RIGHT;
          sideButtonLayout         = doc["sideButtonLayout"]         | (uint8_t)PREV_NEXT;
          frontButtonBack          = doc["frontButtonBack"]          | (uint8_t)FRONT_HW_BACK;
          frontButtonConfirm       = doc["frontButtonConfirm"]       | (uint8_t)FRONT_HW_CONFIRM;
          frontButtonLeft          = doc["frontButtonLeft"]          | (uint8_t)FRONT_HW_LEFT;
          frontButtonRight         = doc["frontButtonRight"]         | (uint8_t)FRONT_HW_RIGHT;
          validateFrontButtonMapping(*this);
          longPressChapterSkip     = doc["longPressChapterSkip"]     | (uint8_t)1;
#ifdef CROSSPOINT_X3
          longPressConfirmAction   = doc["longPressConfirmAction"]   | (uint8_t)6;
#else
          longPressConfirmAction   = doc["longPressConfirmAction"]   | (uint8_t)5;
#endif
          libraryLongPressMenu     = doc["libraryLongPressMenu"]     | (uint8_t)0;
          // Global feature switches
          globalNextPageModeEnabled = doc["globalNextPageModeEnabled"] | (uint8_t)0;
          wifiAlwaysReselect       = doc["wifiAlwaysReselect"]       | (uint8_t)1;
          directTxtRead            = doc["directTxtRead"]            | (uint8_t)1;
          systemLanguage           = doc["systemLanguage"]           | (uint8_t)0;
          if (systemLanguage > 1) systemLanguage = 0;
          // Missing key (old files) → off. Any value other than 1 → off.
          {
            const int raw = doc["developerSerialDebugEnabled"] | 0;
            developerSerialDebugEnabled = (raw == 1) ? 1 : 0;
          }
          getString("libraryHomePath", libraryHomePath, sizeof(libraryHomePath));
          // Auto page turn
          autoPageTurnEnabled      = doc["autoPageTurnEnabled"]      | (uint8_t)0;
          autoPageTurnMode         = doc["autoPageTurnMode"]         | (uint8_t)1;
          autoPageTurnInterval     = doc["autoPageTurnInterval"]     | (uint8_t)10;
          if (autoPageTurnInterval < 1) autoPageTurnInterval = 1;
          if (autoPageTurnInterval > 60) autoPageTurnInterval = 60;
          // Tilt page turn
          tiltPageTurnEnabled      = doc["tiltPageTurnEnabled"]      | (uint8_t)0;
          tiltScope                = doc["tiltScope"]                | (uint8_t)0;
          tiltTriggerAngle         = doc["tiltTriggerAngle"]         | (uint8_t)20;
          if (tiltTriggerAngle < 1) tiltTriggerAngle = 1;
          if (tiltTriggerAngle > 90) tiltTriggerAngle = 90;
          tiltReleaseAngle         = doc["tiltReleaseAngle"]         | (uint8_t)10;
          if (tiltReleaseAngle > 89) tiltReleaseAngle = 89;
          tiltHoldTimeMs           = static_cast<uint16_t>(doc["tiltHoldTimeMs"] | 200);
          if (tiltHoldTimeMs < 50) tiltHoldTimeMs = 50;
          if (tiltHoldTimeMs > 2000) tiltHoldTimeMs = 2000;
          tiltCooldownTimeMs       = static_cast<uint16_t>(doc["tiltCooldownTimeMs"] | 800);
          if (tiltCooldownTimeMs < 100) tiltCooldownTimeMs = 100;
          if (tiltCooldownTimeMs > 5000) tiltCooldownTimeMs = 5000;
          tiltLeftAction           = doc["tiltLeftAction"]           | (uint8_t)0;
          if (tiltLeftAction > 1) tiltLeftAction = 0;
          tiltRightAction          = doc["tiltRightAction"]          | (uint8_t)1;
          if (tiltRightAction > 1) tiltRightAction = 1;
          tiltLargeLeftAction      = doc["tiltLargeLeftAction"]      | (uint8_t)0;
          if (tiltLargeLeftAction > 3) tiltLargeLeftAction = 0;
          tiltLargeRightAction     = doc["tiltLargeRightAction"]     | (uint8_t)1;
          if (tiltLargeRightAction > 3) tiltLargeRightAction = 1;
          // Tap page turn
          tapPageTurnEnabled       = doc["tapPageTurnEnabled"]       | (uint8_t)0;
          tapAction                = doc["tapAction"]                | (uint8_t)1;
          tapThresholdG            = doc["tapThresholdG"]            | (uint8_t)12;
          if (tapThresholdG < 10) tapThresholdG = 10;
          if (tapThresholdG > 80) tapThresholdG = 80;
          tapCooldownMs            = static_cast<uint16_t>(doc["tapCooldownMs"] | 600);
          if (tapCooldownMs < 200) tapCooldownMs = 200;
          if (tapCooldownMs > 3000) tapCooldownMs = 3000;
          // Auto-rotate
          autoRotateEnabled        = doc["autoRotateEnabled"]        | (uint8_t)0;
          // Landscape dual-page
          landscapeDualPageEnabled = doc["landscapeDualPageEnabled"] | (uint8_t)0;
          // Misc
          transparentRemoveWhite   = doc["transparentRemoveWhite"]   | (uint8_t)1;
          // Bluetooth
          bluetoothEnabled         = doc["bluetoothEnabled"]         | (uint8_t)0;
          btKey1Code               = doc["btKey1Code"]               | (uint8_t)0x00;
          btKey1Action             = doc["btKey1Action"]             | (uint8_t)0xFF;
          btKey2Code               = doc["btKey2Code"]               | (uint8_t)0x00;
          btKey2Action             = doc["btKey2Action"]             | (uint8_t)0xFF;
          btKey3Code               = doc["btKey3Code"]               | (uint8_t)0x00;
          btKey3Action             = doc["btKey3Action"]             | (uint8_t)0xFF;
          btKey4Code               = doc["btKey4Code"]               | (uint8_t)0x00;
          btKey4Action             = doc["btKey4Action"]             | (uint8_t)0xFF;
          btKey5Code               = doc["btKey5Code"]               | (uint8_t)0x00;
          btKey5Action             = doc["btKey5Action"]             | (uint8_t)0xFF;
          btKey6Code               = doc["btKey6Code"]               | (uint8_t)0x00;
          btKey6Action             = doc["btKey6Action"]             | (uint8_t)0xFF;
          // Accounts
          getString("opdsServerUrl", opdsServerUrl, sizeof(opdsServerUrl));
          getString("opdsUsername",  opdsUsername,  sizeof(opdsUsername));
          getString("opdsPassword",  opdsPassword,  sizeof(opdsPassword));
          getString("jgUsername",    jgUsername,    sizeof(jgUsername));
          getString("jgAppPassword", jgAppPassword, sizeof(jgAppPassword));
          getString("jgBookFolder",  jgBookFolder,  sizeof(jgBookFolder));
          getString("dcUsername",    dcUsername,    sizeof(dcUsername));
          getString("dcPassword",    dcPassword,    sizeof(dcPassword));
          getString("dcWebdavUrl",   dcWebdavUrl,   sizeof(dcWebdavUrl), "https://data.cstcloud.cn/dav");
          getString("zlibEmail",     zlibEmail,     sizeof(zlibEmail));
          getString("zlibPassword",  zlibPassword,  sizeof(zlibPassword));

          Serial.printf("[%lu] [CPS] Settings loaded from JSON\n", millis());
          return true;
        }
        Serial.printf("[%lu] [CPS] JSON version mismatch: %u\n", millis(), v);
      } else {
        Serial.printf("[%lu] [CPS] JSON parse error: %s\n", millis(), err.c_str());
      }
    }
  }

  // ── 2. 备用：二进制一次性迁移 ─────────────────────────────────────
  if (SdMan.exists(SETTINGS_FILE)) {
    Serial.printf("[%lu] [CPS] Binary settings found, migrating to JSON...\n", millis());
    if (loadFromBinaryFile()) {
      saveToFile();  // 自动转为JSON格式
      Serial.printf("[%lu] [CPS] Migration to JSON complete\n", millis());
      return true;
    }
  }

  return false;
}

float CrossPointSettings::getReaderLineCompression() const {
  // Use customLineSpacing value (5-15 -> 0.5-1.5)
  return static_cast<float>(customLineSpacing) / 10.0f;
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  // If never full refresh is enabled, return a huge number so the counter never triggers
  if (neverFullRefresh) return INT_MAX;
  // Now stores actual page count directly (1-30)
  return refreshFrequency;
}
#include <EpdFontLoader.h>
int CrossPointSettings::getReaderFontId() const {

  if (fontFamily == FONT_CUSTOM) {
    uint8_t targetSize = customFontSize;
    // 0 is automatic; reject values below the supported TTF rasterizer size.
    // This also protects settings written through the web API or old files.
    if (targetSize != 0) {
      targetSize = std::max<uint8_t>(12, std::min<uint8_t>(48, targetSize));
    }
    if (targetSize == 0) {
      switch (fontSize) {
        case SMALL:
          targetSize = 12;
          break;
        case MEDIUM:
        default:
          targetSize = 14;
          break;
        case LARGE:
          targetSize = 16;
          break;
        case EXTRA_LARGE:
          targetSize = 18;
          break;
      }
    }
    int id = EpdFontLoader::getBestFontId(customFontFamily, targetSize);
    if (id != -1) return id;
    static char lastFallbackFamily[sizeof(customFontFamily)] = {};
    static int lastFallbackSize = -1;
    if (lastFallbackSize != targetSize ||
        std::strncmp(lastFallbackFamily, customFontFamily, sizeof(lastFallbackFamily)) != 0) {
      char line[192];
      std::snprintf(line, sizeof(line), "reader_fallback family=%s size=%u id=-1", customFontFamily,
                    static_cast<unsigned>(targetSize));
      FontManager::appendFontDiagnostic(line);
      std::strncpy(lastFallbackFamily, customFontFamily, sizeof(lastFallbackFamily) - 1);
      lastFallbackFamily[sizeof(lastFallbackFamily) - 1] = '\0';
      lastFallbackSize = targetSize;
    }
    // Fallback if custom font not found
  }

  switch (fontFamily) {
    case SYSTEM_FONT:
    default:
      // 系统内置字体：NotoSans
      switch (fontSize) {
        case SMALL:
          return NOTOSANS_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSANS_16_FONT_ID;
        case LARGE:
          return NOTOSANS_18_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
    case FONT_CUSTOM:
      // 外置字体逻辑已在上面处理，这里不应该到达
      return NOTOSANS_16_FONT_ID;
  }
}

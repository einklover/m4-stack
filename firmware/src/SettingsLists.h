#pragma once

#include <vector>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
// All user-facing strings use L(Str::xxx) for i18n support.

inline std::vector<SettingInfo> getSettingsList() {
  return {
      // --- Display ---
      SettingInfo::DynamicEnum(L(Str::kSleepScreen),
        {L(Str::kValDefaultBlack), L(Str::kValDefaultWhite), L(Str::kValTransparentWallpaper),
         L(Str::kValCustomBMP), L(Str::kValCustomImage), L(Str::kValTransparentOverlay)},
        []() -> uint8_t {
          switch (SETTINGS.sleepScreen) {
            case CrossPointSettings::DARK:        return 0;
            case CrossPointSettings::LIGHT:       return 1;
            case CrossPointSettings::MARSK:       return 2;
            case CrossPointSettings::CUSTOM:      return 3;
            case CrossPointSettings::MARSK2:      return 4;
            case CrossPointSettings::TRANSPARENT: return 5;
            default:                              return 1;
          }
        },
        [](uint8_t index) {
          static const uint8_t mapping[] = {
            CrossPointSettings::DARK,
            CrossPointSettings::LIGHT,
            CrossPointSettings::MARSK,
            CrossPointSettings::CUSTOM,
            CrossPointSettings::MARSK2,
            CrossPointSettings::TRANSPARENT
          };
          if (index < 6) {
            SETTINGS.sleepScreen = mapping[index];
            SETTINGS.saveToFile();
          }
        },
        "sleepScreen", "Display"),
    SettingInfo::Enum(
        L(Str::kReadingProgressSetting), &CrossPointSettings::statusBar,
        {L(Str::kValNone), L(Str::kValNoProgress), L(Str::kValFullWithPercent),
         L(Str::kValFullWithBookBar), L(Str::kValOnlyBookBar), L(Str::kValFullWithChapterBar)},
        "statusBar", "Display"),
    SettingInfo::Enum(L(Str::kHideBatteryPercent), &CrossPointSettings::hideBatteryPercentage,
                      {L(Str::kValNever), L(Str::kValInReader), L(Str::kValAlways)},
                      "hideBatteryPercentage", "Display"),
    SettingInfo::Value(L(Str::kRefreshFrequency), &CrossPointSettings::refreshFrequency, 1, 30, 1, "refreshFrequency", "Display"),
    SettingInfo::Toggle(L(Str::kNeverFullRefresh), &CrossPointSettings::neverFullRefresh, "neverFullRefresh", "Display"),
    SettingInfo::Toggle(L(Str::kButtonHints), &CrossPointSettings::buttonHintsEnabled, "buttonHintsEnabled", "Display"),
#ifdef CROSSPOINT_MURPHY_M4
    // Dual-channel frontlight (cool GPIO48 / warm GPIO47). Applied at boot and when changed.
    SettingInfo::Value(L(Str::kFrontlightBrightness), &CrossPointSettings::frontlightBrightness, 0, 100, 5,
                       "frontlightBrightness", "Display"),
    SettingInfo::Value(L(Str::kFrontlightWarmth), &CrossPointSettings::frontlightWarmth, 0, 100, 5, "frontlightWarmth",
                       "Display"),
#endif
    SettingInfo::Toggle(L(Str::kFullRefreshBeforeSleep), &CrossPointSettings::sleepBeforeFullRefresh, "sleepBeforeFullRefresh", "Display"),
    SettingInfo::Enum(L(Str::kImageQuality), &CrossPointSettings::imageQuality,
                      {L(Str::kValFast), L(Str::kValNormal), L(Str::kValHD)},
                      "imageQuality", "Display"),
    SettingInfo::Enum(L(Str::kIconStyle), &CrossPointSettings::iconStyle,
                      {L(Str::kValStyle1), L(Str::kValStyle2), L(Str::kValStyle3)},
                      "iconStyle", "Display"),
    SettingInfo::Enum(L(Str::kIconSelectedStyle), &CrossPointSettings::homeIconStyle,
                      {L(Str::kValCornersOnly), L(Str::kValGrayBgOnly), L(Str::kValDashedBorderOnly),
                       L(Str::kValSolidBorderOnly), L(Str::kValCornersAndGrayBg)},
                      "homeIconStyle", "Display"),

    // --- Reader ---
#if 0  // 字号设置项暂时隐藏，当前只有一种字号
    SettingInfo::DynamicEnum(L(Str::kFontSize), {L(Str::kValSmall), L(Str::kValMedium)},
            []() -> uint8_t {
              switch (SETTINGS.fontSize) {
                case CrossPointSettings::SMALL: return 0;
                case CrossPointSettings::LARGE: return 1;
                default:                        return 1;
              }
            },
            [](uint8_t index) {
              static const uint8_t mapping[] = {
                CrossPointSettings::SMALL,
                CrossPointSettings::LARGE
              };
              if (index < 2) {
                SETTINGS.fontSize = mapping[index];
                SETTINGS.saveToFile();
              }
            },
            "fontSize", "Reader"),
#endif  // 字号设置项隐藏结束
    SettingInfo::Toggle(L(Str::kFirstLineIndent), &CrossPointSettings::firstlineintented, "firstlineintented","Reader"),
    // One family-independent reader body size. Allowed body sizes are exactly
    // {16,24,26,36,38,40,48} (default 26). 32 and 45 are excluded due to
    // the 74/75 kernel split at N=32/45. Snap ties to larger.
    SettingInfo::DynamicEnum(L(Str::kFontSize),
                       {"16", "24", "26", "36", "38", "40", "48"},
                       []() -> uint8_t {
                         uint8_t cur = SETTINGS.getReaderPixelSize();
                         for (uint8_t i = 0; i < CrossPointSettings::kReaderBodyPixelSizesCount; ++i) {
                           if (CrossPointSettings::kReaderBodyPixelSizes[i] == cur) return i;
                         }
                         uint8_t snapped = CrossPointSettings::snapReaderPixelSize(cur);
                         for (uint8_t i = 0; i < CrossPointSettings::kReaderBodyPixelSizesCount; ++i) {
                           if (CrossPointSettings::kReaderBodyPixelSizes[i] == snapped) return i;
                         }
                         return 2;
                       },
                       [](uint8_t index) {
                         if (index < CrossPointSettings::kReaderBodyPixelSizesCount) {
                           SETTINGS.setReaderPixelSize(CrossPointSettings::kReaderBodyPixelSizes[index]);
                           SETTINGS.saveToFile();
                         }
                       },
                       "readerPixelSize", "Reader"),
    SettingInfo::Value(L(Str::kLineSpacing), &CrossPointSettings::customLineSpacing, 5, 20, 1, "lineSpacing", "Reader"),
    SettingInfo::SignedValue(L(Str::kWordSpacing), &CrossPointSettings::wordSpacing, -20, 20, 1, "wordSpacing", "Reader"),
    SettingInfo::Value(L(Str::kTopMargin), &CrossPointSettings::screenMargin_Top, 0,60,1, "screenMarginTop", "Reader"),
    SettingInfo::Value(L(Str::kBottomMargin), &CrossPointSettings::screenMargin_Bottom, 0,60,1, "screenMarginBottom", "Reader"),
    SettingInfo::Value(L(Str::kLeftMargin), &CrossPointSettings::screenMargin_Left, 0,60,1,"screenMarginLeft", "Reader"),
    SettingInfo::Value(L(Str::kRightMargin), &CrossPointSettings::screenMargin_Right, 0,60,1, "screenMarginRight", "Reader"),
    SettingInfo::Toggle(L(Str::kReadingBackground), &CrossPointSettings::ReadingScreenEnabled,"readingBackground","Reader"),
    SettingInfo::Toggle(L(Str::kUnderline), &CrossPointSettings::extraline,"underline","Reader"),
    SettingInfo::SignedValue(L(Str::kUnderlineOffset), &CrossPointSettings::underlineOffset, -30, 30, 1, "underlineOffset", "Reader"),
    SettingInfo::Enum(L(Str::kUnderlineStyle), &CrossPointSettings::underlineStyle,
                      {L(Str::kValSolidLine), L(Str::kValShortDash), L(Str::kValMediumDash),
                       L(Str::kValLongDash), L(Str::kValDotLine)},
                      "underlineStyle", "Reader"),
    SettingInfo::Toggle(L(Str::kParagraphSpacing), &CrossPointSettings::extraParagraphSpacing, "extraParagraphSpacing", "Reader"),
    SettingInfo::Enum(L(Str::kAlignment), &CrossPointSettings::paragraphAlignment,
                      {L(Str::kValJustify), L(Str::kValLeftAlign), L(Str::kValCenterAlign),
                       L(Str::kValRightAlign), L(Str::kValBookStyle)},
                      "alignment", "Reader"),
    SettingInfo::Toggle(L(Str::kShowTime), &CrossPointSettings::showTimeInsteadOfChapter, "showTimeInsteadOfChapter", "Reader"),
    SettingInfo::Toggle(L(Str::kShowEpubImages), &CrossPointSettings::epubShowImages, "showEpubImages", "Reader"),
    SettingInfo::Enum(L(Str::kPunctWidth), &CrossPointSettings::chinesePunctWidth,
                      {L(Str::kValCompact), L(Str::kValStandard)},
                      "punctWidth", "Reader"),
    SettingInfo::Toggle(L(Str::kLandscapeDualPage), &CrossPointSettings::landscapeDualPageEnabled, "landscapeDualPageEnabled", "Reader"),
#ifdef CROSSPOINT_MURPHY_M4
    SettingInfo::Toggle(L(Str::kPageTurnAnimation), &CrossPointSettings::pageTurnAnimationEnabled,
                        "pageTurnAnimationEnabled", "Reader"),
    SettingInfo::Enum(L(Str::kPageTurnAnimDir), &CrossPointSettings::pageTurnAnimationDir,
                      {L(Str::kPageTurnDirR2L), L(Str::kPageTurnDirL2R), L(Str::kPageTurnDirB2T),
                       L(Str::kPageTurnDirT2B)},
                      "pageTurnAnimationDir", "Reader"),
#endif

      // --- Controls ---
      SettingInfo::Enum(L(Str::kSideButtonSettings), &CrossPointSettings::sideButtonLayout,
                        {L(Str::kValUpAndDown), L(Str::kValDownAndUp)}, "sideButtonLayout", "Controls"),
      SettingInfo::Toggle(L(Str::kLongPressChapterSkip), &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          "Controls"),
      SettingInfo::Enum(L(Str::kShortPowerButton), &CrossPointSettings::shortPwrBtn,
                        {L(Str::kValIgnore), L(Str::kValSleep), L(Str::kValPageTurn), L(Str::kValFullRefresh), L(Str::kValConfirm)},
                        "shortPwrBtn", "Controls"),
      SettingInfo::Toggle(L(Str::kLongPressBoot), &CrossPointSettings::longPressBoot, "longPressBoot", "Controls"),
      SettingInfo::Toggle(L(Str::kLongPressConfirmMenu), &CrossPointSettings::libraryLongPressMenu, "libraryLongPressMenu", "Controls"),

#ifdef CROSSPOINT_X3
      SettingInfo::Toggle(L(Str::kTiltEventSwitch), &CrossPointSettings::tiltPageTurnEnabled,
                          "tiltPageTurnEnabled", "Controls"),
      SettingInfo::Enum(L(Str::kTiltScope), &CrossPointSettings::tiltScope,
                        {L(Str::kTiltScopeReaderOnly), L(Str::kTiltScopeGlobal)},
                        "tiltScope", "Controls"),
#if 0  // 敲击翻页设置项暂时隐藏，功能代码保留
      SettingInfo::Toggle(L(Str::kTapPageTurn), &CrossPointSettings::tapPageTurnEnabled,
                          "tapPageTurnEnabled", "Controls"),
      SettingInfo::Enum(L(Str::kTapAction), &CrossPointSettings::tapAction,
                        {L(Str::kTapPrevPage), L(Str::kTapNextPage)},
                        "tapAction", "Controls"),
      SettingInfo::DynamicEnum(L(Str::kTapThreshold),
                               {L(Str::kTapSensLow), L(Str::kTapSensMedLow), L(Str::kTapSensMed), L(Str::kTapSensMedHigh), L(Str::kTapSensHigh)},
                               []() -> uint8_t {
                                 if (SETTINGS.tapThresholdG >= 35) return 0;
                                 if (SETTINGS.tapThresholdG >= 25) return 1;
                                 if (SETTINGS.tapThresholdG >= 18) return 2;
                                 if (SETTINGS.tapThresholdG >= 12) return 3;
                                 return 4;
                               },
                               [](uint8_t index) {
                                 static const uint8_t thresholds[] = {35, 25, 18, 12, 8};
                                 if (index < 5) {
                                   SETTINGS.tapThresholdG = thresholds[index];
                                   SETTINGS.saveToFile();
                                 }
                               },
                               "tapThresholdG", "Controls"),
      SettingInfo::DynamicEnum(L(Str::kTapCooldown),
                               {L(Str::kTapCd300), L(Str::kTapCd400), L(Str::kTapCd600), L(Str::kTapCd800), L(Str::kTapCd1000)},
                               []() -> uint8_t {
                                 if (SETTINGS.tapCooldownMs <= 300) return 0;
                                 if (SETTINGS.tapCooldownMs <= 400) return 1;
                                 if (SETTINGS.tapCooldownMs <= 600) return 2;
                                 if (SETTINGS.tapCooldownMs <= 800) return 3;
                                 return 4;
                               },
                               [](uint8_t index) {
                                 static const uint16_t cooldowns[] = {300, 400, 600, 800, 1000};
                                 if (index < 5) {
                                   SETTINGS.tapCooldownMs = cooldowns[index];
                                   SETTINGS.saveToFile();
                                 }
                               },
                               "tapCooldownMs", "Controls"),
#endif  // 敲击翻页设置项隐藏结束
      SettingInfo::Toggle(L(Str::kAutoRotate), &CrossPointSettings::autoRotateEnabled, "autoRotateEnabled", "Controls"),
#endif  // CROSSPOINT_X3

      // --- System ---
      SettingInfo::Enum(L(Str::kSystemLanguage), &CrossPointSettings::systemLanguage,
                        {L(Str::kLangSimplifiedChinese), L(Str::kLangTraditionalChinese)}, "systemLanguage", "System"),
      SettingInfo::Toggle(L(Str::kAutoSyncTimeOnBoot), &CrossPointSettings::autoSyncTimeOnBoot, "autoSyncTimeOnBoot", "System"),
      SettingInfo::Toggle(L(Str::kWifiAlwaysReselect), &CrossPointSettings::wifiAlwaysReselect, "wifiAlwaysReselect", "System"),
      SettingInfo::Toggle(L(Str::kDirectTxtRead), &CrossPointSettings::directTxtRead, "directTxtRead", "System"),
      SettingInfo::Enum(L(Str::kSleepTimeout), &CrossPointSettings::sleepTimeout,
                        {L(Str::kVal1Min), L(Str::kVal5Min), L(Str::kVal10Min), L(Str::kVal15Min), L(Str::kVal30Min)},
                        "sleepTimeout", "System"),

#ifdef CROSSPOINT_MURPHY_M4
      // System UI wipe (Home / activity entry). Reader page-turn lives in Reader.
      // CRITICAL: pageTurnAnimationFrameRate stores the raw LUT byte (0x22/0x44/0x88),
      // NOT an enum index. Using SettingInfo::Enum with that field OOBs on render
      // (default 0x88 → enumValues[136] → crash when opening System). Map via DynamicEnum.
      SettingInfo::Toggle(L(Str::kSystemAnimation), &CrossPointSettings::systemAnimationEnabled,
                          "systemAnimationEnabled", "System"),
      SettingInfo::Value(L(Str::kPageTurnAnimSteps), &CrossPointSettings::pageTurnAnimationSteps, 2, 64, 1,
                         "pageTurnAnimationSteps", "System"),
      // Window width in step units.
      SettingInfo::Value(L(Str::kPageTurnAnimMult), &CrossPointSettings::pageTurnAnimationMult, 1, 16, 1,
                         "pageTurnAnimationMult", "System"),
      // TP is the LUT phase length nibble (1..16); store as decimal 1..16.
      SettingInfo::Value(L(Str::kPageTurnAnimTp), &CrossPointSettings::pageTurnAnimationTp, 1, 16, 1,
                         "pageTurnAnimationTp", "System"),
      SettingInfo::DynamicEnum(
          L(Str::kPageTurnAnimFrameRate),
          {L(Str::kPageTurnFrSlow), L(Str::kPageTurnFrMed), L(Str::kPageTurnFrFast)},
          []() -> uint8_t {
            switch (SETTINGS.pageTurnAnimationFrameRate) {
              case 0x22: return 0;
              case 0x44: return 1;
              case 0x88: return 2;
              default:   return 2;
            }
          },
          [](uint8_t index) {
            static const uint8_t kRates[] = {0x22, 0x44, 0x88};
            if (index < 3) {
              SETTINGS.pageTurnAnimationFrameRate = kRates[index];
              SETTINGS.saveToFile();
            }
          },
          "pageTurnAnimationFrameRate", "System"),
#endif

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          "KOReader Username", [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", "KOReader Sync"),
      SettingInfo::DynamicString(
          "KOReader Password", [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", "KOReader Sync"),
      SettingInfo::DynamicString(
          "Sync Server URL", [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", "KOReader Sync"),
      SettingInfo::DynamicEnum(
          "Document Matching", {"Filename", "Binary"},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", "KOReader Sync"),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String("OPDS Server URL", SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl), "opdsServerUrl",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Username", SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Password", SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          "OPDS Browser"),

      // --- 坚果云配置 (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String(L(Str::kJianGuoAccount), SETTINGS.jgUsername, sizeof(SETTINGS.jgUsername), "jgUsername",
                          "坚果云配置"),
      SettingInfo::String(L(Str::kJianGuoAppPassword), SETTINGS.jgAppPassword, sizeof(SETTINGS.jgAppPassword), "jgAppPassword",
                          "坚果云配置"),
      SettingInfo::String(L(Str::kJianGuoReadFolder), SETTINGS.jgBookFolder, sizeof(SETTINGS.jgBookFolder), "jgBookFolder",
                          "坚果云配置"),

      // --- 数据胶囊配置 (WebDAV) ---
      SettingInfo::String(L(Str::kDataCapsuleUsername), SETTINGS.dcUsername, sizeof(SETTINGS.dcUsername), "dcUsername",
                          "数据胶囊配置"),
      SettingInfo::String(L(Str::kDataCapsulePassword), SETTINGS.dcPassword, sizeof(SETTINGS.dcPassword), "dcPassword",
                          "数据胶囊配置"),
  };
}

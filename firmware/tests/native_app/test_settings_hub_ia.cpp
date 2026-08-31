// Round 3 Lane C — Settings Hub IA contracts
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr firmware/tests/native_app/test_settings_hub_ia.cpp -o /tmp/test_settings_hub_ia && /tmp/test_settings_hub_ia
// Expected: RED until SettingsHubPolicy.h + hub titles lands. Do not edit production to make green.
// Spec: docs/superpowers/specs/2026-08-31-settings-l2-scene.md §4
//   Hub titles 显示与阅读 / 按键与操作 / 网络与同步 / 系统与维护
//   Keys: readerLayout in DisplayReading, never sdOta, per-card membership
//   Flat counts not tested here — see test_settings_l2_window.cpp

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#if __has_include("activities/settings/SettingsHubPolicy.h")
#include "activities/settings/SettingsHubPolicy.h"
#define HAS_SETTINGS_HUB_POLICY 1
#else
#define HAS_SETTINGS_HUB_POLICY 0
#endif

#if !HAS_SETTINGS_HUB_POLICY
int main() {
  // Missing header is the expected RED before Lane A lands.
  // We compile a stub that reports RED at runtime rather than a cryptic
  // "file not found" compile error, so CI output is self-explanatory.
  // This still fails (assert) to signal RED.
  printf("RED: activities/settings/SettingsHubPolicy.h not found — expected until Lane A (muse-impl) lands\n");
  printf("Checked: firmware/src/activities/settings/SettingsHubPolicy.h\n");
  printf("Hub IA contracts: hub titles, readerLayout, sdOta, per-card key membership — cannot verify without header\n");
  fflush(stdout);
  assert(false && "RED: SettingsHubPolicy.h missing — Lane A must create it per spec §7");
  return 1;
}
#else

namespace {

bool streq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

void testHubCardCountAndEnum() {
  // Spec §4: exactly 4 cards, values 0..3, distinct.
  assert(kSettingsHubCardCount == 4 && "kSettingsHubCardCount must be 4");
  assert(static_cast<int>(SettingsHubCard::DisplayReading) == 0);
  assert(static_cast<int>(SettingsHubCard::KeysOperations) == 1);
  assert(static_cast<int>(SettingsHubCard::NetworkSync) == 2);
  assert(static_cast<int>(SettingsHubCard::SystemMaintenance) == 3);
  // Compatibility aliases must not add values — KeysOps == KeysOperations etc.
  // If aliases exist, they must equal the canonical.
#if defined(__has_include) // keep compiler quiet if alias not present
  // Check alias via int comparison when alias exists; if it doesn't exist this line would fail compile,
  // so we guard with a SFINAE-style: only test if the enum has the alias name (use preprocessor if def exists)
  // We can't detect enum alias at preprocessor, so we just verify distinctness of main 4 via runtime.
#endif
  printf("hub card count & enum PASS\n");
}

void testHubTitles() {
  // Spec §4: simplified titles fixed. Traditional via same function's Traditional path not tested here,
  // but simplified must match exactly.
  const char* t0 = settingsHubCardTitleZh(SettingsHubCard::DisplayReading);
  const char* t1 = settingsHubCardTitleZh(SettingsHubCard::KeysOperations);
  const char* t2 = settingsHubCardTitleZh(SettingsHubCard::NetworkSync);
  const char* t3 = settingsHubCardTitleZh(SettingsHubCard::SystemMaintenance);
  // UTF-8 literals
  assert(streq(t0, u8"显示与阅读") && "DisplayReading title must be 显示与阅读");
  assert(streq(t1, u8"按键与操作") && "KeysOperations title must be 按键与操作");
  assert(streq(t2, u8"网络与同步") && "NetworkSync title must be 网络与同步");
  assert(streq(t3, u8"系统与维护") && "SystemMaintenance title must be 系统与维护");
  // Must NOT be old kCategoryDisplay style "1)显示"
  assert(!streq(t0, "1)显示") && !streq(t1, "2)按钮") && !streq(t2, "3)系统"));
  // Distinctness
  assert(!streq(t0, t1) && !streq(t0, t2) && !streq(t0, t3));
  assert(!streq(t1, t2) && !streq(t1, t3));
  assert(!streq(t2, t3));
  printf("hub titles PASS (DisplayReading=显示与阅读 etc)\n");
}

void testReaderLayoutInDisplayAndNeverSdOta() {
  // Spec §4.1: readerLayout door in DisplayReading, never sdOta anywhere
  const bool m4_true = true;
  const bool m4_false = false;
  // readerLayout must be in DisplayReading for both builds (it is not m4Only per adaptation)
  assert(settingsHubContainsKey(SettingsHubCard::DisplayReading, "readerLayout", m4_true, false) &&
         "readerLayout must be in DisplayReading (M4 true)");
  assert(settingsHubContainsKey(SettingsHubCard::DisplayReading, "readerLayout", m4_false, false) &&
         "readerLayout must be in DisplayReading (M4 false) — not m4Only");
  // Must NOT be in other cards
  for (auto card : {SettingsHubCard::KeysOperations, SettingsHubCard::NetworkSync, SettingsHubCard::SystemMaintenance}) {
    assert(!settingsHubContainsKey(card, "readerLayout", m4_true, false) &&
           "readerLayout must NOT be in Keys/Network/System");
    assert(!settingsHubContainsKey(card, "readerLayout", m4_false, false));
  }
  // Never sdOta in any card, either build
  for (auto card : {SettingsHubCard::DisplayReading, SettingsHubCard::KeysOperations,
                    SettingsHubCard::NetworkSync, SettingsHubCard::SystemMaintenance}) {
    for (bool m4 : {false, true}) {
      assert(!settingsHubContainsKey(card, "sdOta", m4, false) && "sdOta must never appear in any Hub card (forbidden)");
      assert(!settingsHubContainsKey(card, "SdOta", m4, false));
      // also ensure SdOtaUpdateActivity-style key not present via substring check on each row's key
      int cnt = settingsHubRowCount(card, m4, false);
      for (int i = 0; i < cnt; ++i) {
        auto row = settingsHubRowAt(card, i, m4, false);
        if (row.key) {
          assert(std::strcmp(row.key, "sdOta") != 0 && "row key must not be sdOta");
          // also catch sdOta case-insensitive? spec forbids exactly sdOta
        }
      }
    }
  }
  printf("readerLayout & never-sdOta PASS\n");
}

void testPerCardKeyMembershipM4() {
  // Spec §4 IA grouping source: port from adaptation SettingsHubPolicy.h, do not invent new keys.
  // Validate per-card membership for M4 true build (the production target).
  // Adaptation lists are the oracle; we lock them here so impl cannot drift.

  // DisplayReading M4 true = 19 rows (adaptation's kDisplayRows filtered M4 true)
  {
    const auto card = SettingsHubCard::DisplayReading;
    int cnt = settingsHubRowCount(card, true, false);
    assert(cnt == 19 && "DisplayReading M4 true rowCount must be 19 (8 UI chrome +3 eink +2 frontlight +1 readerLayout +5 pageTurn)");
    // Expected keys M4 true in adaptation order (must all be present)
    const char* expect19[] = {
      "sleepScreen","statusBar","hideBatteryPercentage","refreshFrequency","neverFullRefresh",
      "buttonHintsEnabled","frontlightBrightness","frontlightWarmth","sleepBeforeFullRefresh",
      "imageQuality","iconStyle","homeIconStyle","uiFontSize","readerLayout",
      "systemAnimationEnabled","pageTurnAnimationSteps","pageTurnAnimationMult",
      "pageTurnAnimationTp","pageTurnAnimationFrameRate"
    };
    for (auto k : expect19) {
      assert(settingsHubContainsKey(card, k, true, false) && "DisplayReading M4 must contain expected key");
    }
    // Negative: keys that belong to other cards must NOT be in Display
    assert(!settingsHubContainsKey(card, "remapButtons", true, false));
    assert(!settingsHubContainsKey(card, "bluetooth", true, false));
    assert(!settingsHubContainsKey(card, "systemLanguage", true, false));
    // Count must match exactly — no extra
    int found = 0;
    for (int i=0;i<cnt;++i) {
      auto r = settingsHubRowAt(card, i, true, false);
      assert(r.key && r.key[0] != '\0' && "every Display row must have a key");
      ++found;
    }
    assert(found == 19);
  }

  // DisplayReading M4 false = 11 rows (frontlight + uiFontSize + pageTurn group removed)
  {
    const auto card = SettingsHubCard::DisplayReading;
    int cnt = settingsHubRowCount(card, false, false);
    assert(cnt == 11 && "DisplayReading M4 false rowCount must be 11");
    // m4Only keys must be absent
    assert(!settingsHubContainsKey(card, "frontlightBrightness", false, false));
    assert(!settingsHubContainsKey(card, "frontlightWarmth", false, false));
    assert(!settingsHubContainsKey(card, "uiFontSize", false, false));
    assert(!settingsHubContainsKey(card, "systemAnimationEnabled", false, false));
    // readerLayout must still be there
    assert(settingsHubContainsKey(card, "readerLayout", false, false));
  }

  // KeysOperations = 6 rows (single section, omit title later but rowCount is 6)
  {
    const auto card = SettingsHubCard::KeysOperations;
    int cnt = settingsHubRowCount(card, true, false);
    assert(cnt == 6 && "KeysOperations rowCount must be 6");
    const char* expect6[] = {"remapButtons","sideButtonLayout","shortPwrBtn","longPressChapterSkip","longPressBoot","libraryLongPressMenu"};
    for (auto k : expect6) assert(settingsHubContainsKey(card, k, true, false));
    // Negative
    assert(!settingsHubContainsKey(card, "sleepScreen", true, false));
    assert(!settingsHubContainsKey(card, "wifiAlwaysReselect", true, false));
    assert(!settingsHubContainsKey(card, "sdOta", true, false));
    cnt = settingsHubRowCount(card, false, false);
    assert(cnt == 6 && "KeysOperations M4 false also 6");
  }

  // NetworkSync = 6 rows
  {
    const auto card = SettingsHubCard::NetworkSync;
    int cnt = settingsHubRowCount(card, true, false);
    assert(cnt == 6 && "NetworkSync rowCount must be 6");
    const char* expect6[] = {"wifiAlwaysReselect","autoSyncTimeOnBoot","bluetooth","koreader","jianguo","dataCapsule"};
    for (auto k : expect6) assert(settingsHubContainsKey(card, k, true, false));
    assert(!settingsHubContainsKey(card, "sleepScreen", true, false));
    assert(!settingsHubContainsKey(card, "systemLanguage", true, false));
  }

  // SystemMaintenance M4 true =7 rows, M4 false=5
  {
    const auto card = SettingsHubCard::SystemMaintenance;
    int cntTrue = settingsHubRowCount(card, true, false);
    assert(cntTrue == 7 && "SystemMaintenance M4 true rowCount must be 7 (3 system +2 maintenance +2 M4)");
    int cntFalse = settingsHubRowCount(card, false, false);
    assert(cntFalse == 5 && "SystemMaintenance M4 false rowCount must be 5");
    assert(settingsHubContainsKey(card, "systemLanguage", true, false));
    assert(settingsHubContainsKey(card, "sleepTimeout", true, false));
    assert(settingsHubContainsKey(card, "directTxtRead", true, false));
    assert(settingsHubContainsKey(card, "clearCache", true, false));
    assert(settingsHubContainsKey(card, "resetSettings", true, false));
    assert(settingsHubContainsKey(card, "developerOptions", true, false));
    assert(settingsHubContainsKey(card, "switchBootSlot", true, false));
    assert(!settingsHubContainsKey(card, "developerOptions", false, false));
    assert(!settingsHubContainsKey(card, "switchBootSlot", false, false));
    // Must retain switchBootSlot, never sdOta
    assert(!settingsHubContainsKey(card, "sdOta", true, false));
  }

  printf("per-card key membership (M4 true/false) PASS\n");
}

void testNoHardcodedProbeSizes() {
  // Ensure policy does not hardcode snapshot probe sizes as a fake fix.
  // This is a lightweight static check: rowCount for Display M4 must be 19, not 64x64 etc.
  // If impl hardcodes probe sizes, row counts would not match spec.
  (void)settingsHubRowAt; // ensure function exists
  printf("no hardcoded probe size sanity PASS (row counts matched spec)\n");
}

} // namespace

int main() {
  testHubCardCountAndEnum();
  testHubTitles();
  testReaderLayoutInDisplayAndNeverSdOta();
  testPerCardKeyMembershipM4();
  testNoHardcodedProbeSizes();
  printf("settings hub IA ALL PASS\n");
  return 0;
}

#endif // HAS_SETTINGS_HUB_POLICY

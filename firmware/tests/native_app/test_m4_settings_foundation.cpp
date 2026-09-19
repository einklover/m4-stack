// M4 UI Redesign Phase 1 — Foundation contracts
// Build (Foundation worktree root):
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_m4_settings_foundation.cpp \
//     -o /tmp/test_m4_settings_foundation && /tmp/test_m4_settings_foundation
// Expected RED until M4Settings*.h + M4NetworkOccupancy.h land and
// SettingsActivity::toggleCurrentSetting resolves by key.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if __has_include("activities/settings/M4SettingsAction.h")
#include "activities/settings/M4SettingsAction.h"
#define HAS_ACTION 1
#else
#define HAS_ACTION 0
#endif

#if __has_include("activities/settings/M4SettingsCatalog.h")
#include "activities/settings/M4SettingsCatalog.h"
#define HAS_CATALOG 1
#else
#define HAS_CATALOG 0
#endif

#if __has_include("activities/settings/M4SettingsNav.h")
#include "activities/settings/M4SettingsNav.h"
#define HAS_NAV 1
#else
#define HAS_NAV 0
#endif

#if __has_include("activities/settings/M4SettingsFocus.h")
#include "activities/settings/M4SettingsFocus.h"
#define HAS_FOCUS 1
#else
#define HAS_FOCUS 0
#endif

#if __has_include("activities/settings/M4SettingsConfirm.h")
#include "activities/settings/M4SettingsConfirm.h"
#define HAS_CONFIRM 1
#else
#define HAS_CONFIRM 0
#endif

#if __has_include("network/M4NetworkOccupancy.h")
#include "network/M4NetworkOccupancy.h"
#define HAS_OCC 1
#else
#define HAS_OCC 0
#endif

#if __has_include("activities/settings/SettingsHubPolicy.h")
#include "activities/settings/SettingsHubPolicy.h"
#define HAS_HUB 1
#else
#define HAS_HUB 0
#endif

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string loadSettingsActivityCpp() {
  const char* candidates[] = {
      "firmware/src/activities/settings/SettingsActivity.cpp",
      "./firmware/src/activities/settings/SettingsActivity.cpp",
      "../firmware/src/activities/settings/SettingsActivity.cpp",
      "../../firmware/src/activities/settings/SettingsActivity.cpp",
  };
  for (auto p : candidates) {
    std::string c = readFile(p);
    if (!c.empty()) {
      printf("loaded SettingsActivity.cpp from %s (%zu bytes)\n", p, c.size());
      return c;
    }
  }
  return {};
}

std::string stripComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool inStr = false, inChr = false, inLine = false, inBlock = false;
  for (size_t i = 0; i < text.size();) {
    char c = text[i];
    if (inLine) {
      out.push_back(c);
      if (c == '\n') inLine = false;
      ++i;
      continue;
    }
    if (inBlock) {
      if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
        out.push_back(' ');
        out.push_back(' ');
        i += 2;
        inBlock = false;
        continue;
      }
      out.push_back(c == '\n' ? '\n' : ' ');
      ++i;
      continue;
    }
    if (inStr) {
      out.push_back(c);
      if (c == '\\' && i + 1 < text.size()) {
        out.push_back(text[i + 1]);
        i += 2;
        continue;
      }
      if (c == '"') inStr = false;
      ++i;
      continue;
    }
    if (inChr) {
      out.push_back(c);
      if (c == '\\' && i + 1 < text.size()) {
        out.push_back(text[i + 1]);
        i += 2;
        continue;
      }
      if (c == '\'') inChr = false;
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      inLine = true;
      out.push_back(' ');
      out.push_back(' ');
      i += 2;
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      inBlock = true;
      out.push_back(' ');
      out.push_back(' ');
      i += 2;
      continue;
    }
    if (c == '"') inStr = true;
    if (c == '\'') inChr = true;
    out.push_back(c);
    ++i;
  }
  return out;
}

std::string extractFn(const std::string& src, const char* sig) {
  auto pos = src.find(sig);
  if (pos == std::string::npos) return {};
  auto brace = src.find('{', pos);
  if (brace == std::string::npos) return {};
  int depth = 0;
  for (size_t i = brace; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    else if (src[i] == '}') {
      --depth;
      if (depth == 0) return src.substr(brace, i - brace + 1);
    }
  }
  return {};
}

bool streq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

}  // namespace

#if !(HAS_ACTION && HAS_CATALOG && HAS_NAV && HAS_FOCUS && HAS_CONFIRM && HAS_OCC)

int main() {
  printf("RED: Foundation headers missing (action=%d catalog=%d nav=%d focus=%d confirm=%d occ=%d)\n",
         HAS_ACTION, HAS_CATALOG, HAS_NAV, HAS_FOCUS, HAS_CONFIRM, HAS_OCC);
  printf("Need: M4SettingsCatalog/Action/Nav/Focus/Confirm.h and network/M4NetworkOccupancy.h\n");
  fflush(stdout);
  assert(false && "RED: Foundation headers not yet present");
  return 1;
}

#else

namespace {

void testRootCatalog() {
  const M4SettingsRow* rows = m4SettingsRootCatalog();
  assert(rows);
  assert(kM4SettingsRootCount == 8);
  const char* expect[] = {"wifi",         "frontlight", "readerLayout", "sleepTimeout",
                          "sleepScreen",  "keys",       "maintenance",  "advanced"};
  for (int i = 0; i < 8; ++i) {
    assert(streq(rows[i].key, expect[i]));
    assert(rows[i].titleZh && rows[i].titleZh[0]);
    assert(rows[i].actionable);
  }
  assert(m4SettingsIndexOfKey(rows, 8, "readerLayout") == 2);
  assert(m4SettingsIndexOfKey(rows, 8, "nope") == -1);
  const M4SettingsRow* wifi = m4SettingsRowByKey(rows, 8, "wifi");
  assert(wifi && wifi->control == M4SettingsControl::Navigate);
  const M4SettingsRow* sleep = m4SettingsRowByKey(rows, 8, "sleepTimeout");
  assert(sleep && sleep->control == M4SettingsControl::Choice);
  printf("root catalog PASS\n");
}

void testIndexStableAfterShuffle() {
  M4SettingsRow copy[8];
  const M4SettingsRow* src = m4SettingsRootCatalog();
  for (int i = 0; i < 8; ++i) copy[i] = src[i];
  std::reverse(copy, copy + 8);
  assert(m4SettingsIndexOfKey(copy, 8, "wifi") == 7);
  assert(m4SettingsIndexOfKey(copy, 8, "advanced") == 0);
  assert(streq(m4SettingsRowByKey(copy, 8, "keys")->key, "keys"));
  printf("indexOfKey shuffle PASS\n");
}

void testActivateByKeyNotVisualIndex() {
  // DisplayReading: flatten visual setting index 3 is buttonHintsEnabled;
  // getSettingsList insertion store index 3 is refreshFrequency (R1).
  const char* visual[] = {"sleepScreen", "statusBar", "hideBatteryPercentage", "buttonHintsEnabled",
                          "imageQuality"};
  const char* store[] = {"sleepScreen", "statusBar", "hideBatteryPercentage", "refreshFrequency",
                         "neverFullRefresh", "buttonHintsEnabled"};
  const int selectedVisual = 3;
  assert(!streq(visual[selectedVisual], store[selectedVisual]));
  int storeIdx = -1;
  for (int i = 0; i < 6; ++i) {
    if (streq(store[i], visual[selectedVisual])) storeIdx = i;
  }
  // Helper must resolve by key, not visual index.
  int resolved = m4SettingsStoreIndexForKey(store, 6, visual[selectedVisual]);
  assert(resolved == storeIdx);
  assert(resolved == 5);
  assert(resolved != selectedVisual);
  assert(streq(store[resolved], "buttonHintsEnabled"));
  printf("activate-by-key (visual3!=store3) PASS\n");
}

void testPolicyKeyBridge() {
#if HAS_HUB
  const char* k = m4SettingsPolicyKeyAt(SettingsHubCard::DisplayReading, 3, true);
  // insertion-order policy index 3 is refreshFrequency (kDisplayRows).
  assert(streq(k, "refreshFrequency"));
  int policyOfHints = m4SettingsPolicyIndexOfKey(SettingsHubCard::DisplayReading, "buttonHintsEnabled", true);
  assert(policyOfHints == 5);
  int flattenSlot = 0;
  int flatCount = settingsFlatCount(SettingsHubCard::DisplayReading, true);
  const char* flatten3 = nullptr;
  for (int f = 0; f < flatCount; ++f) {
    auto row = settingsFlatAt(SettingsHubCard::DisplayReading, f, true);
    if (row.kind != SettingsFlatKind::Setting) continue;
    if (flattenSlot == 3) {
      flatten3 = row.key;
      break;
    }
    ++flattenSlot;
  }
  assert(streq(flatten3, "buttonHintsEnabled"));
  printf("policy/flatten key bridge PASS (policy3=%s flatten3=%s)\n", k, flatten3);
#else
  printf("policy key bridge SKIP (no hub policy)\n");
#endif
}

void testNoWrap() {
  assert(m4SettingsNavMoveClamp(0, -1, 8) == 0);
  assert(m4SettingsNavMoveClamp(7, 1, 8) == 7);
  assert(m4SettingsNavMoveClamp(3, 1, 8) == 4);
  assert(m4SettingsNavMoveClamp(3, -1, 8) == 2);
  assert(m4SettingsNavMoveClamp(0, 0, 8) == 0);
  assert(m4SettingsNavMoveClamp(0, -1, 0) == 0);
  printf("no-wrap nav PASS\n");
}

void testControlMappingAndFooter() {
  // 0 TOGGLE, 1 ENUM, 2 ACTION, 3 VALUE, 4 STRING
  assert(m4SettingsControlForKind(0, 2, false, false) == M4SettingsControl::Toggle);
  assert(m4SettingsControlForKind(1, 2, false, false) == M4SettingsControl::Toggle);
  assert(m4SettingsControlForKind(1, 3, false, false) == M4SettingsControl::Choice);
  assert(m4SettingsControlForKind(3, 0, false, false) == M4SettingsControl::Number);
  assert(m4SettingsControlForKind(2, 0, true, false) == M4SettingsControl::Confirm);
  assert(m4SettingsControlForKind(2, 0, false, true) == M4SettingsControl::Navigate);
  assert(streq(m4SettingsFooterConfirmLabel(M4SettingsControl::Navigate), "打开"));
  assert(streq(m4SettingsFooterConfirmLabel(M4SettingsControl::Toggle), "切换"));
  assert(streq(m4SettingsFooterConfirmLabel(M4SettingsControl::Choice), "选择"));
  assert(streq(m4SettingsFooterConfirmLabel(M4SettingsControl::Number), "选择"));
  assert(streq(m4SettingsFooterConfirmLabel(M4SettingsControl::Confirm), "打开"));
  assert(m4AutoConnectKnownNetworksFromAlwaysReselect(1) == 0);
  assert(m4AutoConnectKnownNetworksFromAlwaysReselect(0) == 1);
  printf("control/footer/auto-connect PASS\n");
}

void testFocusDirty() {
  M4FocusRect bar = m4SettingsFocusBarLocal();
  assert(bar.x == 0 && bar.y == 12 && bar.w == 8 && bar.h == 56);
  assert(kM4PartialBurst == 6);
  const int itemH = 80, gap = 4;
  M4DirtyUnion d = m4SettingsDirtyAfterMove(0, 1, itemH, gap, 0);
  assert(!d.full);
  // rows 0 and 1: y 68-origin is applied by caller; local bands [0,80) and [84,164)
  assert(d.y0 == 0);
  assert(d.y1 == 80 + 4 + 80);
  M4DirtyUnion full = m4SettingsDirtyAfterMove(1, 2, itemH, gap, 6);
  assert(full.full);
  M4DirtyUnion stillPartial = m4SettingsDirtyAfterMove(1, 2, itemH, gap, 5);
  assert(!stillPartial.full);
  printf("focus/dirty PASS\n");
}

void testConfirmPower() {
  assert(m4SettingsPowerIsPrimary(M4ConfirmButton::Power) == false);
  assert(m4SettingsPowerIsPrimary(M4ConfirmButton::Confirm) == false);
  assert(!m4SettingsDangerAccepts(M4ConfirmButton::Power, true));
  assert(!m4SettingsDangerAccepts(M4ConfirmButton::Confirm, false));
  assert(m4SettingsDangerAccepts(M4ConfirmButton::Confirm, true));
  assert(m4SettingsDangerAccepts(M4ConfirmButton::Other, true));  // footer primary / on-page row
  assert(!m4SettingsDangerAccepts(M4ConfirmButton::Back, true));
  printf("confirm/power PASS\n");
}

void testOccupancy() {
  m4NetworkOccupancyResetForTest();
  assert(m4NetworkRadioMayOff());
  assert(m4NeedNetwork(M4NetworkOwner::Transfer) == M4NetworkAcquireResult::Ok);
  assert(m4NetworkHeldBy(M4NetworkOwner::Transfer));
  assert(!m4NetworkRadioMayOff());
  // NTP/云盘/OTA/传书 share the radio; a second NeedNetwork is additive.
  assert(m4NeedNetwork(M4NetworkOwner::Ntp) == M4NetworkAcquireResult::Ok);
  assert(m4NetworkHeldBy(M4NetworkOwner::Ntp));
  // Steal (force radio off) is denied while any other owner still holds.
  assert(m4NetworkTryDeinit(M4NetworkOwner::WifiSettings) == M4NetworkAcquireResult::DeniedHeldByOther);
  m4ReleaseNetwork(M4NetworkOwner::Ntp);
  assert(m4NetworkHeldBy(M4NetworkOwner::Transfer));
  assert(!m4NetworkRadioMayOff());
  m4ReleaseNetwork(M4NetworkOwner::Transfer);
  assert(m4NetworkRadioMayOff());
  assert(m4NetworkTryDeinit(M4NetworkOwner::WifiSettings) == M4NetworkAcquireResult::Ok);
  assert(m4NeedNetwork(M4NetworkOwner::WifiSettings) == M4NetworkAcquireResult::Ok);
  m4ReleaseNetwork(M4NetworkOwner::WifiSettings);
  assert(m4NetworkRadioMayOff());
  printf("occupancy PASS\n");
}

void testToggleResolvesByKey() {
  std::string src = loadSettingsActivityCpp();
  assert(!src.empty() && "SettingsActivity.cpp must be readable");
  std::string body = extractFn(stripComments(src), "SettingsActivity::toggleCurrentSetting");
  assert(!body.empty() && "toggleCurrentSetting body not found");
  const bool usesVecIdx =
      body.find("const auto &setting = vec[idx]") != std::string::npos ||
      body.find("const auto& setting = vec[idx]") != std::string::npos;
  const bool usesKey =
      body.find("findSettingByKey") != std::string::npos ||
      body.find("m4SettingsPolicyKeyAt") != std::string::npos ||
      body.find("m4SettingsRowByKey") != std::string::npos;
  printf("toggle identity: usesVecIdx=%d usesKey=%d\n", (int)usesVecIdx, (int)usesKey);
  assert(!usesVecIdx && "toggleCurrentSetting must not identity-activate vec[idx]");
  assert(usesKey && "toggleCurrentSetting must resolve the setting by key");
  printf("toggleCurrentSetting key identity PASS\n");
}

}  // namespace

int main() {
  testRootCatalog();
  testIndexStableAfterShuffle();
  testActivateByKeyNotVisualIndex();
  testPolicyKeyBridge();
  testNoWrap();
  testControlMappingAndFooter();
  testFocusDirty();
  testConfirmPower();
  testOccupancy();
  testToggleResolvesByKey();
  printf("ALL Foundation contracts PASS\n");
  return 0;
}

#endif

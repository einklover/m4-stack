// Phase 1 Settings UI — root IA contracts (retired 4-card Hub).
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_settings_hub_ia.cpp \
//     -o /tmp/test_settings_hub_ia && /tmp/test_settings_hub_ia
// Spec: docs/superpowers/specs/2026-09-13-m4-ui-redesign-apple.md §4.1

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("activities/settings/M4SettingsCatalog.h")
#include "activities/settings/M4SettingsCatalog.h"
#define HAS_CATALOG 1
#else
#define HAS_CATALOG 0
#endif

#if __has_include("activities/settings/M4SettingsRootUi.h")
#include "activities/settings/M4SettingsRootUi.h"
#define HAS_ROOT 1
#else
#define HAS_ROOT 0
#endif

#if __has_include("activities/settings/SettingsSceneModel.h")
#include "activities/settings/SettingsSceneModel.h"
#define HAS_MODEL 1
#else
#define HAS_MODEL 0
#endif

namespace {

bool streq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

#if !(HAS_CATALOG && HAS_ROOT)

int main() {
  printf("RED: 8-row Settings root APIs missing — Hub 4-card IA retired\n");
  fflush(stdout);
  assert(false && "RED: Settings root catalog/UI not present");
  return 1;
}

#else

void testRootEightRowsNotFourCards() {
  assert(kM4SettingsRootCount == 8);
  const char* expect[] = {"wifi",        "frontlight", "readerLayout", "sleepTimeout",
                          "sleepScreen", "keys",       "maintenance",  "advanced"};
  const char* titles[] = {"Wi-Fi", "前光", "阅读", "休眠", "锁屏", "按键", "维护", "更多/高级"};
  const M4SettingsRow* rows = m4SettingsRootCatalog();
  for (int i = 0; i < 8; ++i) {
    assert(streq(rows[i].key, expect[i]));
    assert(rows[i].actionable);
    assert(streq(rows[i].titleZh, titles[i]));
  }
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  assert(st.page == M4SettingsPageKind::Root);
  assert(m4SettingsUiVisibleCount(st) == 8);
  printf("root 8-row IA (not 4 Hub cards) PASS\n");
}

void testReaderLayoutOnRootNeverSdOta() {
  const M4SettingsRow* rows = m4SettingsRootCatalog();
  bool hasReader = false;
  for (int i = 0; i < kM4SettingsRootCount; ++i) {
    assert(rows[i].key);
    assert(std::strcmp(rows[i].key, "sdOta") != 0);
    if (streq(rows[i].key, "readerLayout")) hasReader = true;
  }
  assert(hasReader);
  std::string src;
  const char* cands[] = {
      "firmware/src/activities/settings/SettingsActivity.cpp",
      "./firmware/src/activities/settings/SettingsActivity.cpp",
  };
  for (auto p : cands) {
    src = readFile(p);
    if (!src.empty()) break;
  }
  assert(!src.empty());
  assert(src.find("sdOta") == std::string::npos);
  assert(src.find("SdOta") == std::string::npos);
  assert(src.find("openHubCard") == std::string::npos);
  assert(src.find("populateHubFromPolicy") == std::string::npos);
  printf("readerLayout on root / never sdOta / no Hub open PASS\n");
}

void testChildMembership() {
  assert(m4SettingsChildCount("frontlight") == 2);
  assert(streq(m4SettingsChildAt("keys", 0)->key, "remapButtons"));
  assert(m4SettingsChildCount("keys") == 6);
  assert(m4SettingsChildCount("maintenance") >= 2);
  bool hasClear = false, hasReset = false;
  for (int i = 0; i < m4SettingsChildCount("maintenance"); ++i) {
    const char* k = m4SettingsChildAt("maintenance", i)->key;
    if (streq(k, "clearCache")) hasClear = true;
    if (streq(k, "resetSettings")) hasReset = true;
    assert(!streq(k, "sdOta"));
  }
  assert(hasClear && hasReset);
  assert(m4SettingsChildCount("advanced") > 8);
  printf("child membership PASS\n");
}

void testSceneNotHub() {
#if HAS_MODEL
  SettingsScene::SettingsSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.populateRootFromCatalog("wifi", "未连接", "", "", "", "", "", ""));
  assert(model.publish());
  SettingsScene::SettingsSnapshot snap{};
  assert(model.copyLatest(snap));
  assert(snap.pane != SettingsPane::Hub);
  assert(snap.windowCount == 8);
  assert(model.bindingSource(snap).size(SettingsScene::kBindingHubCards) == 0);
  printf("scene model not Hub pane PASS\n");
#else
  assert(false && "SettingsSceneModel.h missing");
#endif
}

int main() {
  testRootEightRowsNotFourCards();
  testReaderLayoutOnRootNeverSdOta();
  testChildMembership();
  testSceneNotHub();
  printf("settings hub IA ALL PASS (8-row root, Hub retired)\n");
  return 0;
}

#endif

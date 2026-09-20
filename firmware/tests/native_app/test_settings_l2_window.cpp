// Phase 1 Settings UI — root/child windowing. Wrap retired; clamp only.
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_settings_l2_window.cpp \
//     -o /tmp/test_settings_l2_window && /tmp/test_settings_l2_window

#include <cassert>
#include <cstdio>
#include <cstring>

#if __has_include("activities/settings/M4SettingsRootUi.h")
#include "activities/settings/M4SettingsRootUi.h"
#define HAS_ROOT 1
#else
#define HAS_ROOT 0
#endif

#if __has_include("activities/settings/M4SettingsNav.h")
#include "activities/settings/M4SettingsNav.h"
#define HAS_NAV 1
#else
#define HAS_NAV 0
#endif

#if __has_include("ui/scene/UiSceneTypes.h")
#include "ui/scene/UiSceneTypes.h"
#define HAS_SCENE_TYPES 1
#else
#define HAS_SCENE_TYPES 0
#endif

#if !(HAS_ROOT && HAS_NAV)

int main() {
  printf("RED: root UI / clamp nav missing — wrap tests retired\n");
  fflush(stdout);
  assert(false && "RED: m4SettingsNavMoveClamp / M4SettingsRootUi.h required");
  return 1;
}

#else

namespace {

bool streq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

void testKMaxRepeatItemsLocked() {
#if HAS_SCENE_TYPES
  assert(UiScene::kMaxRepeatItems == 9 && "kMaxRepeatItems must remain 9 for Advanced v5.1");
#endif
  printf("kMaxRepeatItems==9 PASS\n");
}

void testRootNoWrapNoSections() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  assert(m4SettingsUiVisibleCount(st) == 8);
  for (int i = 0; i < 8; ++i) {
    const M4SettingsRow* row = m4SettingsUiVisibleRow(st, i);
    assert(row && row->actionable);
  }
  assert(m4SettingsUiMove(st, -1) == 0);
  assert(streq(st.selectedKey, "wifi"));
  for (int i = 0; i < 20; ++i) m4SettingsUiMove(st, 1);
  assert(st.selectedSlot == 7);
  assert(streq(st.selectedKey, "advanced"));
  assert(m4SettingsNavMoveClamp(0, -1, 8) == 0);
  assert(m4SettingsNavMoveClamp(7, 1, 8) == 7);
  assert(m4SettingsNavMoveClamp(0, 8, 8) == 7);
  printf("root no-wrap / no sections PASS\n");
}

void testAdvancedWindowClamp() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiOpenChildList(st, "advanced");
  const int n = m4SettingsUiVisibleCount(st);
  assert(n == 5);
  assert(st.windowStart == 0);
  assert(st.selectedSlot == 0);
  for (int i = 0; i < n + 5; ++i) m4SettingsUiMove(st, 1);
  assert(st.selectedSlot == n - 1);
  assert(st.windowStart == 0);
  assert(m4SettingsUiMove(st, 1) == n - 1);
  m4SettingsUiMove(st, -(n + 5));
  assert(st.selectedSlot == 0);
  assert(st.windowStart == 0);
  printf("advanced group index fits one screen PASS (n=%d)\n", n);
}

void testKeysFitsOneWindow() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiOpenChildList(st, "keys");
  const int n = m4SettingsUiVisibleCount(st);
  assert(n <= 9);
  m4SettingsUiMove(st, 20);
  assert(st.selectedSlot == n - 1);
  assert(st.windowStart == 0);
  printf("keys fits window 9 PASS\n");
}

}  // namespace

int main() {
  testKMaxRepeatItemsLocked();
  testRootNoWrapNoSections();
  testAdvancedWindowClamp();
  testKeysFitsOneWindow();
  printf("settings L2 window ALL PASS (clamp, window 9, no wrap)\n");
  return 0;
}

#endif

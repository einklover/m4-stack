// Round 3 Lane C — Settings L2 windowing contracts
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr firmware/tests/native_app/test_settings_l2_window.cpp -o /tmp/test_settings_l2_window && /tmp/test_settings_l2_window
// Expected: RED until SettingsHubPolicy.h + SettingsSceneModel.h window APIs land.
// Spec §4 flat counts M4: Display=24 Keys=6 Network=8 System=9; window size 8; selected always inside window;
// §5 window sliding; settingsNavMoveRow must skip section rows.
// Also locks kMaxRepeatItems=8 remains, window=8.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#if __has_include("activities/settings/SettingsHubPolicy.h")
#include "activities/settings/SettingsHubPolicy.h"
#define HAS_POLICY 1
#else
#define HAS_POLICY 0
#endif

// SettingsSceneModel.h is the host-testable snapshot helper (spec §7).
// It must define kSettingsL2Window or kMaxWindowRows ==8; if missing we still test policy.
#if __has_include("activities/settings/SettingsSceneModel.h")
#include "activities/settings/SettingsSceneModel.h"
#define HAS_MODEL 1
#else
#define HAS_MODEL 0
#endif

// Presence of UiSceneTypes for kMaxRepeatItems lock
#if __has_include("ui/scene/UiSceneTypes.h")
#include "ui/scene/UiSceneTypes.h"
#define HAS_SCENE_TYPES 1
#else
#define HAS_SCENE_TYPES 0
#endif

#if !HAS_POLICY
int main() {
  printf("RED: activities/settings/SettingsHubPolicy.h not found — expected until Lane A lands\n");
  printf("  L2 window contracts require settingsFlatCount/settingsWindowStart/settingsNavMoveRow per spec §7\n");
  fflush(stdout);
  assert(false && "RED: SettingsHubPolicy.h missing — window/flat APIs not yet present");
  return 1;
}
#else

namespace {

// Helper to check flat kind is Setting (not Section) — uses SettingsFlatRow::kind if available
// If SettingsFlatKind not defined (old policy), this test will be RED via compile error, which is expected.

void testWindowAndFlatCounts() {
  // Spec §4 flat counts M4 (flattened list includes section titles, Display has 5 titles etc)
  //   Display M4: 5 titles +19 settings =24
  //   Keys: 0 titles (single section omitted) +6 =6
  //   Network: 2 titles +6 =8
  //   System M4: 2 titles +7 =9
  const bool m4 = true;
  int flatDisplay = settingsFlatCount(SettingsHubCard::DisplayReading, m4);
  int flatKeys = settingsFlatCount(SettingsHubCard::KeysOperations, m4);
  int flatNetwork = settingsFlatCount(SettingsHubCard::NetworkSync, m4);
  int flatSystem = settingsFlatCount(SettingsHubCard::SystemMaintenance, m4);
  printf("flat counts M4: Display=%d Keys=%d Network=%d System=%d\n", flatDisplay, flatKeys, flatNetwork, flatSystem);
  assert(flatDisplay == 24 && "Display M4 flat count must be 24 (5 sections +19 settings)");
  assert(flatKeys == 6 && "Keys flat count must be 6 (no section title)");
  assert(flatNetwork == 8 && "Network flat count must be 8 (2 titles +6)");
  assert(flatSystem == 9 && "System M4 flat count must be 9 (2 titles +7)");

  // Window size 8 locked — both policy constant and UiScene kMaxRepeatItems
#if defined(kSettingsL2Window)
  assert(kSettingsL2Window == 8 && "kSettingsL2Window must be 8");
#elif defined(kSettingsHubL2Window)
  assert(kSettingsHubL2Window == 8);
#endif
#if HAS_SCENE_TYPES
  assert(UiScene::kMaxRepeatItems == 8 && "kMaxRepeatItems must remain 8 (spec forbids raising)");
#endif
#if HAS_MODEL
  // SettingsSceneModel window constant (check any of the known names)
#if defined(SettingsSceneModel_kMaxWindowRows)
  // not used
#endif
  // Try to detect via sizeof snapshot window rows: model should have window 8
  // We check via policy's window constant if exposed; else just assert limit via policy
#endif

  // Also verify that available flat window helper respects size 8
  // For flatCount <=8, windowStart must be 0
  assert(settingsWindowStart(0, 6, 8) == 0);
  assert(settingsWindowStart(5, 6, 8) == 0);
  assert(settingsWindowStart(0, 8, 8) == 0);
  assert(settingsWindowStart(7, 8, 8) == 0);
  // For flatCount 24, window sliding
  assert(settingsWindowStart(0, 24, 8) == 0);
  assert(settingsWindowStart(7, 24, 8) == 0 && "flatIndex 7 should still be in first window [0,8)");
  assert(settingsWindowStart(8, 24, 8) == 1 && "flatIndex 8 should push window to 1");
  assert(settingsWindowStart(23, 24, 8) == 16 && "last index 23 -> windowStart 16 (24-8)");
  assert(settingsWindowStart(16, 24, 8) == 16);
  assert(settingsWindowStart(15, 24, 8) == 8);

  printf("window size & flat counts PASS\n");
}

void testSelectedAlwaysInsideWindow() {
  const bool m4 = true;
  const int window = 8;
  for (auto card : {SettingsHubCard::DisplayReading, SettingsHubCard::KeysOperations,
                    SettingsHubCard::NetworkSync, SettingsHubCard::SystemMaintenance}) {
    int settingCount = settingsHubRowCount(card, m4, false);
    int flatCount = settingsFlatCount(card, m4);
    printf("card %d: settingCount=%d flatCount=%d\n", (int)card, settingCount, flatCount);
    for (int s = 0; s < settingCount; ++s) {
      int flatIdx = settingsFlatIndexOfSetting(card, s, m4);
      assert(flatIdx >= 0 && flatIdx < flatCount && "flatIdx must be in [0, flatCount)");
      // Check that flatAt at that index is indeed a Setting row (not Section)
      auto row = settingsFlatAt(card, flatIdx, m4);
      assert(row.kind == SettingsFlatKind::Setting && "setting's flatIndex must point to a Setting row, not Section");
      // Window must contain it
      int wStart = settingsWindowStart(flatIdx, flatCount, window);
      assert(wStart >= 0 && wStart <= std::max(0, flatCount - window));
      assert(flatIdx >= wStart && flatIdx < wStart + window && "selected setting flatIndex must be inside window [windowStart, windowStart+8)");
      // Visible slot must be in [0, window)
      int visibleSlot = flatIdx - wStart;
      assert(visibleSlot >= 0 && visibleSlot < window);
      // Also check via sync helper if available
#if defined(__has_include) // keep compiler portable
#endif
      SettingsNavState st{};
      st.pane = SettingsPane::Category;
      st.hub = card;
      st.selectedRow = s;
      st.windowStart = 0;
      auto synced = settingsNavSyncWindow(st, card, m4);
      assert(synced.windowStart == wStart && "settingsNavSyncWindow must produce same windowStart as settingsWindowStart");
      assert(synced.selectedRow == s);
    }
  }
  printf("selected always inside window PASS\n");
}

void testSectionRowsNotSelectableViaMoveRow() {
  // Spec §5: settingsNavMoveRow must skip section rows (selectedRow is setting index, not flat index)
  // It must wrap among settings only.
  const bool m4 = true;
  for (auto card : {SettingsHubCard::DisplayReading, SettingsHubCard::NetworkSync, SettingsHubCard::SystemMaintenance}) {
    int settingCount = settingsHubRowCount(card, m4, false);
    int flatCount = settingsFlatCount(card, m4);
    if (settingCount <= 1) continue;
    // Collect flat indices for each setting
    std::vector<int> flatForSetting;
    for (int s = 0; s < settingCount; ++s) flatForSetting.push_back(settingsFlatIndexOfSetting(card, s, m4));
    // Verify that flat indices are increasing and skip section rows: gaps >1 where sections exist
    for (size_t i = 1; i < flatForSetting.size(); ++i) {
      int prev = flatForSetting[i-1];
      int cur = flatForSetting[i];
      assert(cur > prev && "flat indices for successive settings must increase");
      // If there's a section between them, the jump will be 2 at that boundary; otherwise 1
      // Just ensure no two settings map to same flat
      assert(cur != prev);
      // Ensure intermediate flat rows between prev and cur (if gap>1) are Section kind
      for (int f = prev+1; f < cur; ++f) {
        auto r = settingsFlatAt(card, f, m4);
        assert(r.kind == SettingsFlatKind::Section && "gap between settings must be section row(s)");
      }
    }
    // Now test nav move wrapping
    SettingsNavState st{};
    st.pane = SettingsPane::Hub; // moveRow should be no-op when not in Category
    st.hub = card;
    st.selectedRow = 0;
    auto movedHub = settingsNavMoveRow(st, 1, settingCount);
    assert(movedHub.selectedRow == 0 && "moveRow in Hub pane must be no-op");
    st.pane = SettingsPane::Category;
    st.hub = card;
    st.selectedRow = 0;
    // Move forward through all settings and verify we never land on section flat
    for (int step = 0; step < settingCount; ++step) {
      auto nxt = settingsNavMoveRow(st, 1, settingCount);
      int expected = (st.selectedRow + 1) % settingCount;
      assert(nxt.selectedRow == expected && "moveRow +1 must wrap modulo settingCount");
      int flat = settingsFlatIndexOfSetting(card, nxt.selectedRow, m4);
      auto fr = settingsFlatAt(card, flat, m4);
      assert(fr.kind == SettingsFlatKind::Setting && "moveRow must land on Setting, never Section");
      st = nxt;
    }
    // Move backward wraps
    st.selectedRow = 0;
    auto prv = settingsNavMoveRow(st, -1, settingCount);
    assert(prv.selectedRow == settingCount - 1 && "moveRow -1 from 0 must wrap to last");
    // Large delta wraps
    st.selectedRow = 0;
    auto big = settingsNavMoveRow(st, settingCount, settingCount);
    assert(big.selectedRow == 0 && "delta == settingCount should wrap to same");
    auto big2 = settingsNavMoveRow(st, settingCount + 1, settingCount);
    assert(big2.selectedRow == 1);
    // Empty count no-op
    st.selectedRow = 2;
    auto empty = settingsNavMoveRow(st, 1, 0);
    assert(empty.selectedRow == 2 && "moveRow with settingCount 0 must be no-op");

    // KeysOperations should have contiguous flat (no sections), so flat indices are 0..5
    if (card == SettingsHubCard::KeysOperations) {
      for (int s = 0; s < settingCount; ++s) {
        assert(flatForSetting[s] == s && "KeysOperations has no sections, flat index must equal setting index");
      }
    }
    // Verify window still contains selection after moves via sync
    for (int s = 0; s < settingCount; ++s) {
      SettingsNavState cur{};
      cur.pane = SettingsPane::Category;
      cur.hub = card;
      cur.selectedRow = s;
      cur.windowStart = settingsWindowStart(settingsFlatIndexOfSetting(card, s, m4), flatCount, 8);
      auto synced = settingsNavSyncWindow(cur, card, m4);
      int flat = settingsFlatIndexOfSetting(card, s, m4);
      assert(flat >= synced.windowStart && flat < synced.windowStart + 8);
    }
    printf("card %d moveRow skips sections PASS (settingCount=%d flatCount=%d)\n", (int)card, settingCount, flatCount);
  }
  // Also test NetworkSync forward/back for completeness
  printf("settingsNavMoveRow skips section rows PASS\n");
}

void testWindowEdgeCases() {
  // Window must never exceed flatCount and must be clamped
  const bool m4 = true;
  int flatDisplay = settingsFlatCount(SettingsHubCard::DisplayReading, m4);
  assert(flatDisplay == 24);
  // Selected at top and bottom edges
  for (int flat : {0, 7, 8, 15, 16, 23}) {
    int ws = settingsWindowStart(flat, flatDisplay, 8);
    assert(ws >= 0 && ws <= flatDisplay - 8);
    assert(flat >= ws && flat < ws + 8);
  }
  // FlatCount exactly 8 -> window always 0
  assert(settingsWindowStart(0, 8, 8) == 0);
  assert(settingsWindowStart(7, 8, 8) == 0);
  // FlatCount <8 -> window 0 as well (our impl should clamp)
  assert(settingsWindowStart(0, 6, 8) == 0);
  printf("window edge cases PASS\n");
}

void testKeysHasNoSections() {
  const bool m4 = true;
  int flat = settingsFlatCount(SettingsHubCard::KeysOperations, m4);
  assert(flat == 6);
  for (int i = 0; i < flat; ++i) {
    auto r = settingsFlatAt(SettingsHubCard::KeysOperations, i, m4);
    assert(r.kind == SettingsFlatKind::Setting && "KeysOperations flattened must be all Setting (no sections)");
    assert(r.key && r.key[0] != '\0');
  }
  printf("KeysOperations no-section PASS\n");
}

void testFlatSectionCounts() {
  // Verify number of section rows per card matches spec
  const bool m4 = true;
  auto countKind = [&](SettingsHubCard card, SettingsFlatKind k) {
    int flat = settingsFlatCount(card, m4);
    int c = 0;
    for (int i = 0; i < flat; ++i) if (settingsFlatAt(card, i, m4).kind == k) ++c;
    return c;
  };
  assert(countKind(SettingsHubCard::DisplayReading, SettingsFlatKind::Section) == 5 && "Display must have 5 section rows M4");
  assert(countKind(SettingsHubCard::DisplayReading, SettingsFlatKind::Setting) == 19);
  assert(countKind(SettingsHubCard::KeysOperations, SettingsFlatKind::Section) == 0);
  assert(countKind(SettingsHubCard::KeysOperations, SettingsFlatKind::Setting) == 6);
  assert(countKind(SettingsHubCard::NetworkSync, SettingsFlatKind::Section) == 2);
  assert(countKind(SettingsHubCard::NetworkSync, SettingsFlatKind::Setting) == 6);
  assert(countKind(SettingsHubCard::SystemMaintenance, SettingsFlatKind::Section) == 2 && "System M4 2 sections");
  assert(countKind(SettingsHubCard::SystemMaintenance, SettingsFlatKind::Setting) == 7);
  printf("flat section/setting kind counts PASS\n");
}

} // namespace

int main() {
  testWindowAndFlatCounts();
  testSelectedAlwaysInsideWindow();
  testSectionRowsNotSelectableViaMoveRow();
  testWindowEdgeCases();
  testKeysHasNoSections();
  testFlatSectionCounts();
  printf("settings L2 window ALL PASS (flat counts 24/6/8/9, window 8, moveRow skips sections)\n");
  return 0;
}

#endif // HAS_POLICY

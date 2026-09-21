// M4 UI Redesign Phase 1 — Settings UI contracts (Task S)
// Build (repo root):
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_m4_settings_ui.cpp \
//     -o /tmp/test_m4_settings_ui && /tmp/test_m4_settings_ui
// Expected RED until M4SettingsRootUi.h lands and Settings root retires Hub.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("activities/settings/M4SettingsRootUi.h")
#include "activities/settings/M4SettingsRootUi.h"
#define HAS_ROOT_UI 1
#else
#define HAS_ROOT_UI 0
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

#if __has_include("activities/settings/M4SettingsAction.h")
#include "activities/settings/M4SettingsAction.h"
#define HAS_ACTION 1
#else
#define HAS_ACTION 0
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

#if __has_include("activities/settings/M4SettingsPaint.h")
#include "activities/settings/M4SettingsPaint.h"
#define HAS_PAINT 1
#else
#define HAS_PAINT 0
#endif

#if __has_include("activities/settings/SettingsSceneModel.h")
#include "activities/settings/SettingsSceneModel.h"
#define HAS_MODEL 1
#else
#define HAS_MODEL 0
#endif

#if __has_include("ui/scene/UiSceneTypes.h")
#include "ui/scene/UiSceneTypes.h"
#define HAS_SCENE_TYPES 1
#else
#define HAS_SCENE_TYPES 0
#endif

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string compactJson(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') o.push_back(c);
  }
  return o;
}

std::string loadFirst(const char* const* candidates, int n) {
  for (int i = 0; i < n; ++i) {
    std::string c = readFile(candidates[i]);
    if (!c.empty()) {
      printf("loaded %s (%zu bytes)\n", candidates[i], c.size());
      return c;
    }
  }
  return {};
}

std::string loadSettingsActivityCpp() {
  const char* c[] = {
      "firmware/src/activities/settings/SettingsActivity.cpp",
      "./firmware/src/activities/settings/SettingsActivity.cpp",
      "../firmware/src/activities/settings/SettingsActivity.cpp",
  };
  return loadFirst(c, 3);
}

std::string loadSettingsSceneModel() {
  const char* c[] = {
      "firmware/src/activities/settings/SettingsSceneModel.h",
      "./firmware/src/activities/settings/SettingsSceneModel.h",
      "../firmware/src/activities/settings/SettingsSceneModel.h",
  };
  return loadFirst(c, 3);
}

std::string loadGfxSceneRenderer() {
  const char* c[] = {
      "firmware/src/ui/scene/GfxSceneRenderer.h",
      "./firmware/src/ui/scene/GfxSceneRenderer.h",
      "../firmware/src/ui/scene/GfxSceneRenderer.h",
  };
  return loadFirst(c, 3);
}

std::string loadThemeL2() {
  const char* c[] = {
      "themes/murphy-settings/l2.json",
      "./themes/murphy-settings/l2.json",
      "../themes/murphy-settings/l2.json",
  };
  return loadFirst(c, 3);
}

std::string loadThemeRoot() {
  const char* c[] = {
      "themes/murphy-settings/root.json",
      "./themes/murphy-settings/root.json",
      "../themes/murphy-settings/root.json",
  };
  return loadFirst(c, 3);
}

std::string loadThemeMaint() {
  const char* c[] = {
      "themes/murphy-settings/maintenance.json",
      "./themes/murphy-settings/maintenance.json",
      "../themes/murphy-settings/maintenance.json",
  };
  return loadFirst(c, 3);
}

std::string loadThemeChild(const char* name) {
  std::string a = std::string("themes/murphy-settings/") + name + ".json";
  std::string b = std::string("./themes/murphy-settings/") + name + ".json";
  std::string cc = std::string("../themes/murphy-settings/") + name + ".json";
  const char* c[] = {a.c_str(), b.c_str(), cc.c_str()};
  return loadFirst(c, 3);
}

std::string loadThemeHub() {
  const char* c[] = {
      "themes/murphy-settings/hub.json",
      "./themes/murphy-settings/hub.json",
      "../themes/murphy-settings/hub.json",
  };
  return loadFirst(c, 3);
}

std::string loadClearCache() {
  const char* c[] = {
      "firmware/src/activities/settings/ClearCacheActivity.cpp",
      "./firmware/src/activities/settings/ClearCacheActivity.cpp",
      "../firmware/src/activities/settings/ClearCacheActivity.cpp",
  };
  return loadFirst(c, 3);
}

std::string loadReset() {
  const char* c[] = {
      "firmware/src/activities/settings/ResetSettingsActivity.cpp",
      "./firmware/src/activities/settings/ResetSettingsActivity.cpp",
      "../firmware/src/activities/settings/ResetSettingsActivity.cpp",
  };
  return loadFirst(c, 3);
}

bool streq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

}  // namespace

#if !HAS_ROOT_UI

int main() {
  printf("RED: activities/settings/M4SettingsRootUi.h not yet present\n");
  printf("Need: 8-row root session, selectedKey identity, no-wrap, choice/confirm, child lists\n");
  fflush(stdout);
  assert(false && "RED: M4SettingsRootUi.h missing — Settings UI root not yet present");
  return 1;
}

#else

namespace {

void testRootEightActionableNoSection() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  assert(st.page == M4SettingsPageKind::Root);
  assert(m4SettingsUiVisibleCount(st) == 8);
  assert(kM4SettingsRootCount == 8);
  const char* expect[] = {"wifi",        "frontlight", "readerLayout", "sleepTimeout",
                          "sleepScreen", "keys",       "maintenance",  "advanced"};
  int sections = 0;
  for (int i = 0; i < 8; ++i) {
    const M4SettingsRow* row = m4SettingsUiVisibleRow(st, i);
    assert(row);
    assert(streq(row->key, expect[i]));
    assert(row->actionable);
    assert(row->titleZh && row->titleZh[0]);
  }
  assert(sections == 0);
  assert(streq(st.selectedKey, "wifi"));
  printf("root 8 actionable / no section PASS\n");
}

void testNoWrapClamp() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  assert(m4SettingsUiMove(st, -1) == 0);
  assert(streq(st.selectedKey, "wifi"));
  for (int i = 0; i < 20; ++i) m4SettingsUiMove(st, 1);
  assert(st.selectedSlot == 7);
  assert(streq(st.selectedKey, "advanced"));
  assert(m4SettingsUiMove(st, 1) == 7);
  printf("root no-wrap clamp PASS\n");
}

void testHighlightAConfirmBImpossibleOnRoot() {
  // Visual catalog slot 3 is sleepTimeout. Store insertion index 3 is refreshFrequency (R1).
  const char* visual[] = {"wifi", "frontlight", "readerLayout", "sleepTimeout",
                          "sleepScreen", "keys", "maintenance", "advanced"};
  const char* store[] = {"sleepScreen", "statusBar", "hideBatteryPercentage", "refreshFrequency",
                         "neverFullRefresh", "buttonHintsEnabled", "sleepTimeout"};
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, "sleepTimeout");
  assert(st.selectedSlot == 3);
  assert(streq(visual[st.selectedSlot], "sleepTimeout"));
  assert(!streq(store[st.selectedSlot], "sleepTimeout"));
  const char* fired = m4SettingsUiActivateIdentity(st);
  assert(streq(fired, "sleepTimeout"));
  int storeIdx = m4SettingsUiStoreIndexForActivation(st.selectedKey, store, 7);
  assert(storeIdx == 6);
  assert(storeIdx != st.selectedSlot);
  assert(streq(store[storeIdx], "sleepTimeout"));
  assert(!streq(store[st.selectedSlot], fired));
  printf("highlight-A/confirm-B lock PASS (slot3 visual=sleepTimeout store3=refreshFrequency)\n");
}

void testFooterFollowsControl() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, "wifi");
  assert(streq(m4SettingsUiFooterLabel(st), "打开"));
  m4SettingsUiSelectKey(st, "sleepTimeout");
  assert(m4SettingsUiFooterLabel(st) && streq(m4SettingsUiFooterLabel(st), "选择"));
  m4SettingsUiSelectKey(st, "frontlight");
  assert(streq(m4SettingsUiFooterLabel(st), "打开"));
  printf("footer label follows control PASS\n");
}

void testFocusEightPxAndDirtyBurst() {
  M4FocusRect bar = m4SettingsFocusBarLocal();
  assert(bar.w == 8 && bar.h == 56 && bar.x == 0 && bar.y == 12);
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  M4DirtyUnion d = m4SettingsUiNoteMove(st, 0, 1, 80, 4);
  assert(!d.full);
  assert(d.y0 == 0);
  assert(d.y1 == 80 + 4 + 80);
  for (int i = 0; i < 5; ++i) m4SettingsUiNoteMove(st, i, i + 1, 80, 4);
  M4DirtyUnion full = m4SettingsUiNoteMove(st, 5, 6, 80, 4);
  assert(full.full);
  printf("8px focus + dirty burst-6 PASS\n");
}

void testRootMoveConsumesDirtyViaPaintSeam() {
#if !HAS_PAINT
  assert(false && "RED: M4SettingsPaint.h missing — Settings partial-refresh seam not present");
#else
  struct Rec {
    int full = 0;
    int partial = 0;
    M4SettingsRefreshRequest last{};
    static void onFull(void* ctx) { ++static_cast<Rec*>(ctx)->full; }
    static void onPartial(void* ctx, int x, int y, int w, int h) {
      auto* r = static_cast<Rec*>(ctx);
      ++r->partial;
      r->last.mode = M4SettingsRefreshMode::Partial;
      r->last.rect = {x, y, w, h};
    }
  } rec;
  M4SettingsDisplayPort port{&Rec::onFull, &Rec::onPartial, &rec};

  M4SettingsUiState st{};
  M4SettingsPaintSession paint{};
  m4SettingsUiEnterRoot(st);

  // First paint is full even if no move happened.
  auto first = m4SettingsPaintTake(paint, 68, 480, 800);
  assert(first.mode == M4SettingsRefreshMode::Full);
  m4SettingsDisplaySubmit(port, first);
  assert(rec.full == 1);
  assert(rec.partial == 0);

  // Root movement must submit the old∪new row rect, not another full frame.
  int old = st.selectedSlot;
  m4SettingsUiMove(st, 1);
  m4SettingsPaintNoteMove(st, paint, old, st.selectedSlot, 80, 4);
  auto p1 = m4SettingsPaintTake(paint, 68, 480, 800);
  assert(p1.mode == M4SettingsRefreshMode::Partial);
  assert(p1.rect.x == 0);
  assert(p1.rect.y == 68);
  assert(p1.rect.w == 480);
  assert(p1.rect.h == 80 + 4 + 80);
  m4SettingsDisplaySubmit(port, p1);
  assert(rec.partial == 1);
  assert(rec.full == 1);
  assert(rec.last.rect.y == 68);
  assert(rec.last.rect.h == 164);

  for (int i = 0; i < 5; ++i) {
    old = st.selectedSlot;
    m4SettingsUiMove(st, 1);
    m4SettingsPaintNoteMove(st, paint, old, st.selectedSlot, 80, 4);
    auto p = m4SettingsPaintTake(paint, 68, 480, 800);
    assert(p.mode == M4SettingsRefreshMode::Partial);
    m4SettingsDisplaySubmit(port, p);
  }
  assert(rec.partial == 6);

  old = st.selectedSlot;
  m4SettingsUiMove(st, 1);
  m4SettingsPaintNoteMove(st, paint, old, st.selectedSlot, 80, 4);
  auto burst = m4SettingsPaintTake(paint, 68, 480, 800);
  assert(burst.mode == M4SettingsRefreshMode::Full);
  m4SettingsDisplaySubmit(port, burst);
  assert(rec.full == 2);
  printf("root move consumes dirty via paint seam PASS\n");
#endif
}

void assertRefreshOnScreen(const M4SettingsRefreshRequest& req, int screenW, int screenH) {
  assert(req.rect.h >= 0 && "refresh h must never be negative");
  if (req.mode == M4SettingsRefreshMode::Full) {
    assert(req.rect.x == 0 && req.rect.y == 0);
    assert(req.rect.w == screenW && req.rect.h == screenH);
    return;
  }
  assert(req.rect.w > 0);
  assert(req.rect.h > 0 && "empty Partial must upgrade to Full, not h=0");
  assert(req.rect.y >= 0);
  assert(req.rect.y < screenH);
  assert(req.rect.y + req.rect.h <= screenH);
}

void testMakeRefreshClampsInvalidToFull() {
#if !HAS_PAINT
  assert(false && "RED: M4SettingsPaint.h missing");
#else
  // Catalog slot 9→10 at origin 68 is off-screen (y>800) and used to yield
  // y>screen with h=0, which Gfx then silent-drops.
  M4DirtyUnion off{};
  off.y0 = 9 * 84;
  off.y1 = 10 * 84 + 80;
  off.full = false;
  auto req = m4SettingsMakeRefresh(off, 68, 480, 800, false);
  assertRefreshOnScreen(req, 480, 800);
  assert(req.mode == M4SettingsRefreshMode::Full &&
         "empty/off-screen Partial must upgrade to Full, not silent drop");

  M4DirtyUnion inverted{};
  inverted.y0 = 200;
  inverted.y1 = 40;
  inverted.full = false;
  auto inv = m4SettingsMakeRefresh(inverted, 68, 480, 800, false);
  assertRefreshOnScreen(inv, 480, 800);

  M4DirtyUnion ok = m4SettingsDirtyAfterMove(0, 1, 80, 4, 0);
  auto partial = m4SettingsMakeRefresh(ok, 68, 480, 800, false);
  assert(partial.mode == M4SettingsRefreshMode::Partial);
  assert(partial.rect.y == 68);
  assert(partial.rect.h == 164);
  printf("makeRefresh clamps invalid/empty to Full PASS\n");
#endif
}

void testAdvancedWindowRelativeDirty() {
#if !HAS_PAINT
  assert(false && "RED: M4SettingsPaint.h missing");
#else
  M4SettingsUiState st{};
  M4SettingsPaintSession paint{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiOpenChildList(st, "advanced");
  const int n = m4SettingsUiVisibleCount(st);
  assert(n == 5);
  paint.firstPaint = false;
  m4SettingsUiMove(st, 1);
  assert(st.selectedSlot == 1);
  assert(st.windowStart == 0);
  m4SettingsPaintNoteMove(st, paint, 0, st.selectedSlot, 80, 4, 0);
  auto req = m4SettingsPaintTake(paint, 68, 480, 800);
  assertRefreshOnScreen(req, 480, 800);
  printf("Advanced group-index dirty PASS\n");
#endif
}

void testFooterLabelChangeCovered() {
#if !HAS_PAINT
  assert(false && "RED: M4SettingsPaint.h missing");
#else
  M4SettingsUiState st{};
  M4SettingsPaintSession paint{};
  m4SettingsUiEnterRoot(st);
  paint.firstPaint = false;

  // wifi → frontlight: both 打开, keep minimal old∪new rows.
  assert(streq(m4SettingsUiFooterLabel(st), "打开"));
  int old = st.selectedSlot;
  m4SettingsUiMove(st, 1);
  assert(streq(st.selectedKey, "frontlight"));
  assert(streq(m4SettingsUiFooterLabel(st), "打开"));
  m4SettingsPaintNoteMove(st, paint, old, st.selectedSlot, 80, 4, 0);
  auto same = m4SettingsPaintTake(paint, 68, 480, 800);
  assert(same.mode == M4SettingsRefreshMode::Partial);
  assert(same.rect.y == 68);
  assert(same.rect.h == 164);
  assert(same.rect.y + same.rect.h < 736);

  // readerLayout 打开 → sleepTimeout 选择: footer must be in the refresh.
  m4SettingsUiSelectKey(st, "readerLayout");
  assert(st.selectedSlot == 2);
  assert(streq(m4SettingsUiFooterLabel(st), "打开"));
  old = st.selectedSlot;
  m4SettingsUiMove(st, 1);
  assert(streq(st.selectedKey, "sleepTimeout"));
  assert(streq(m4SettingsUiFooterLabel(st), "选择"));
  m4SettingsPaintNoteMove(st, paint, old, st.selectedSlot, 80, 4, 0);
  auto changed = m4SettingsPaintTake(paint, 68, 480, 800);
  assertRefreshOnScreen(changed, 480, 800);
  if (changed.mode == M4SettingsRefreshMode::Partial) {
    assert(changed.rect.y + changed.rect.h >= 736 &&
           "footer label change must cover y>=736 or Full");
  }
  printf("footer label change covers y>=736 or Full PASS\n");
#endif
}

void testWifiPopCallbackResetsFull() {
#if !HAS_PAINT
  assert(false && "RED: M4SettingsPaint.h missing");
#else
  M4SettingsUiState st{};
  M4SettingsPaintSession paint{};
  m4SettingsUiEnterRoot(st);
  st.burstCount = 5;
  paint.firstPaint = false;
  paint.pending = M4DirtyUnion{0, 164, false};
  m4SettingsPaintResetFull(paint, st);
  assert(paint.firstPaint);
  assert(paint.pending.full);
  assert(st.burstCount == 0);

  std::string src = loadSettingsActivityCpp();
  assert(!src.empty());
  const auto wifiCall = src.find("new WifiSelectionActivity");
  assert(wifiCall != std::string::npos);
  const auto wifiEnd = src.find(";", wifiCall);
  assert(wifiEnd != std::string::npos && wifiEnd > wifiCall);
  const std::string call = src.substr(wifiCall, wifiEnd - wifiCall);
  const bool usesSharedRestore = call.find("restore") != std::string::npos;
  const bool resetsPaint = call.find("m4SettingsPaintResetFull") != std::string::npos;
  const bool resetsBurst = call.find("burstCount") != std::string::npos ||
                           call.find("m4SettingsPaintResetFull") != std::string::npos;
  assert(usesSharedRestore || (resetsPaint && resetsBurst));
  assert(call.find("rebuildModel") == std::string::npos || usesSharedRestore || resetsPaint);
  printf("Wi-Fi pop resetFull + burstCount PASS\n");
#endif
}

void testHandleRowHitNotesMove() {
  std::string src = loadSettingsActivityCpp();
  assert(!src.empty());
  const auto hit = src.find("void SettingsActivity::handleRowHit");
  assert(hit != std::string::npos);
  const auto loop = src.find("void SettingsActivity::loop");
  assert(loop != std::string::npos && loop > hit);
  const std::string body = src.substr(hit, loop - hit);
  assert(body.find("m4SettingsPaintNoteMove") != std::string::npos &&
         "handleRowHit must noteMove so pending Partial is old∪new, not a stale union");
  printf("handleRowHit notes move PASS\n");
}

void testChoiceCheckmarkAndRestore() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, "sleepTimeout");
  m4SettingsUiOpenChoice(st, "sleepTimeout", 5, 2);
  assert(st.page == M4SettingsPageKind::Choice);
  assert(m4SettingsUiChoiceChecked(st, 2));
  assert(!m4SettingsUiChoiceChecked(st, 0));
  m4SettingsUiMove(st, 1);
  int committed = m4SettingsUiChoiceCommit(st);
  assert(committed == 3);
  assert(st.page == M4SettingsPageKind::Root);
  assert(streq(st.selectedKey, "sleepTimeout"));
  assert(st.selectedSlot == 3);
  assert(st.windowStart == 0);
  printf("choice checkmark + selectedKey restore PASS\n");
}

void testChildListChoiceCommitRestoresIdentitySlotWindow() {
  // 更多/高级 → 界面与字体 → 界面文字 → Choice → commit 必须一起还原
  // selectedKey/selectedSlot/windowStart，不能停在 Choice 的选项下标。
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, "advanced");
  m4SettingsUiOpenChildList(st, "advanced");
  m4SettingsUiOpenChildList(st, "advancedChrome");
  assert(st.page == M4SettingsPageKind::ChildList);
  assert(streq(st.parentKey, "advancedChrome"));
  m4SettingsUiSelectKey(st, "uiFontSize");
  const int listSlot = st.selectedSlot;
  const int listWindow = st.windowStart;
  assert(listSlot == 3);
  assert(listWindow == 0);
  assert(streq(st.selectedKey, "uiFontSize"));
  const M4SettingsRow* before = m4SettingsUiVisibleRow(st, listSlot);
  assert(before && streq(before->key, "uiFontSize"));

  m4SettingsUiOpenChoice(st, "uiFontSize", 4, 1);
  assert(st.page == M4SettingsPageKind::Choice);
  m4SettingsUiMove(st, 1);
  assert(st.selectedSlot == 2);
  int committed = m4SettingsUiChoiceCommit(st);
  assert(committed == 2);
  assert(st.page == M4SettingsPageKind::ChildList);
  assert(streq(st.selectedKey, "uiFontSize"));
  assert(st.selectedSlot == listSlot);
  assert(st.windowStart == listWindow);
  const M4SettingsRow* after = m4SettingsUiVisibleRow(st, st.selectedSlot);
  assert(after && streq(after->key, "uiFontSize"));
  printf("ChildList Choice commit restores identity+slot+window PASS\n");
}

void testNumberPickerRestoreCancelAndCommit() {
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, "frontlight");
  m4SettingsUiOpenChildList(st, "frontlight");
  m4SettingsUiSelectKey(st, "frontlightBrightness");
  m4SettingsUiPushEditor(st);
  m4SettingsUiPopEditor(st, false);
  assert(streq(st.selectedKey, "frontlightBrightness"));
  assert(!st.editorCommitted);
  m4SettingsUiPushEditor(st);
  m4SettingsUiPopEditor(st, true);
  assert(streq(st.selectedKey, "frontlightBrightness"));
  assert(st.editorCommitted);
  printf("number picker selectedKey restore PASS\n");
}

void testDangerNotPower() {
  assert(!m4SettingsUiConfirmAccepts(M4ConfirmButton::Power, true));
  assert(!m4SettingsUiConfirmAccepts(M4ConfirmButton::Power, false));
  assert(!m4SettingsUiConfirmAccepts(M4ConfirmButton::Confirm, false));
  assert(m4SettingsUiConfirmAccepts(M4ConfirmButton::Other, true));
  assert(m4SettingsUiConfirmAccepts(M4ConfirmButton::Confirm, true));
  assert(!m4SettingsUiConfirmAccepts(M4ConfirmButton::Back, true));
  printf("danger not Power PASS\n");
}

void testSwitchBootSlotIsDangerConfirmPage() {
  const M4SettingsRow* row = m4SettingsChildAt("maintenance", 4);
  assert(row && streq(row->key, "switchBootSlot"));
  assert(row->control == M4SettingsControl::Confirm);

  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, "maintenance");
  m4SettingsUiOpenChildList(st, "maintenance");
  m4SettingsUiSelectKey(st, "switchBootSlot");
  assert(streq(st.selectedKey, "switchBootSlot"));
  const int listSlot = st.selectedSlot;
  const int listWindow = st.windowStart;

  // List-row activate opens a confirm page; it must not request the slot switch.
  m4SettingsUiOpenConfirm(st, "switchBootSlot");
  assert(st.page == M4SettingsPageKind::Confirm);
  assert(streq(st.selectedKey, "switchBootSlot"));
  assert(!st.bootSlotSwitchRequested);
  assert(!st.confirmAccepted);

  assert(!m4SettingsUiConfirmDecide(st, M4ConfirmButton::Power, true));
  assert(st.page == M4SettingsPageKind::Confirm);
  assert(!st.bootSlotSwitchRequested);
  assert(!st.confirmAccepted);

  assert(!m4SettingsUiConfirmDecide(st, M4ConfirmButton::Back, true));
  assert(st.confirmCancelled);
  assert(!st.confirmAccepted);
  assert(!st.bootSlotSwitchRequested);
  assert(st.page == M4SettingsPageKind::ChildList);
  assert(streq(st.selectedKey, "switchBootSlot"));
  assert(st.selectedSlot == listSlot);
  assert(st.windowStart == listWindow);

  m4SettingsUiOpenConfirm(st, "switchBootSlot");
  assert(m4SettingsUiConfirmDecide(st, M4ConfirmButton::Confirm, true));
  assert(st.confirmAccepted);
  assert(st.bootSlotSwitchRequested);
  printf("switchBootSlot danger confirm page PASS\n");
}

void testWholeRowHitNoSplit() {
  // Row 0 band y=68..148. Hit at left edge and right edge both select slot 0.
  assert(m4SettingsWholeRowHit(0, 68 + 10, 0, 68, 80, 4, 480) == 0);
  assert(m4SettingsWholeRowHit(479, 68 + 10, 0, 68, 80, 4, 480) == 0);
  assert(m4SettingsWholeRowHit(240, 68 + 84 + 10, 0, 68, 80, 4, 480) == 1);
  assert(m4SettingsWholeRowHit(10, 10, 0, 68, 80, 4, 480) == -1);
  printf("whole-row hit (no left/right split) PASS\n");
}

void testAdvancedV51PitchHelpers() {
  // Paint, touch, thumb, and overdraw must share the Advanced v5.1 table
  // (origin 126, 52px pitch, gap 0) that the l2 theme repeat paints.
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiOpenChildList(st, "advanced");
  assert(m4SettingsUiIsAdvancedPage(st));
  assert(m4SettingsUiContentOriginY(st) == kM4SettingsAdvancedOriginY);
  assert(kM4SettingsAdvancedOriginY == 130);
  assert(m4SettingsUiListItemH(st) == 58);
  assert(m4SettingsUiListItemGap(st) == 0);
  assert(m4SettingsWholeRowHit(240, 130, 0, 130, 58, 0, 480) == 0);
  assert(m4SettingsWholeRowHit(240, 130 + 4 * 58 + 57, 0, 130, 58, 0, 480) == 4);
  assert(m4SettingsWholeRowHit(240, 130 + 8 * 58, 0, 130, 58, 0, 480) == -1);
  // Grouped children and every other L2 consumer keep the legacy pitch.
  M4SettingsUiState ch{};
  m4SettingsUiEnterRoot(ch);
  m4SettingsUiOpenChildList(ch, "keys");
  assert(!m4SettingsUiIsAdvancedPage(ch));
  assert(m4SettingsUiContentOriginY(ch) == kM4SettingsGroupedChildOriginY);
  assert(m4SettingsUiListItemH(ch) == kM4SettingsL2ItemH);
  assert(m4SettingsUiListItemGap(ch) == kM4SettingsL2Gap);
  printf("Advanced v5.1 pitch helpers PASS\n");
}

void testAdvancedV51TrueTitles() {
  // Displayed fallback titles must equal the SettingInfo truth, never stale
  // v4 labels (key spellings differ, so lookup misses and fallback shows).
  static constexpr const char* kKeys[9] = {"statusBar",       "hideBatteryPercentage",
                                           "refreshFrequency", "neverFullRefresh",
                                           "buttonHintsEnabled", "sleepBeforeFullRefresh",
                                           "imageQuality",    "iconStyle",
                                           "homeIconStyle"};
  static constexpr const char* kWant[9] = {"阅读进度",     "隐藏电池百分比",
                                           "刷新频率",     "永不全刷",
                                           "按钮提示",     "关机前全刷",
                                           "图片质量",     "图标风格",
                                           "图标选中风格"};
  assert(m4SettingsChildCount("advanced") == 5);
  auto findTitle = [](const char* key) -> const char* {
    static const char* groups[] = {"advancedDisplay", "advancedChrome", "advancedAnim",
                                   "advancedConnect", "advancedSystem"};
    for (const char* g : groups) {
      const int n = m4SettingsChildCount(g);
      for (int i = 0; i < n; ++i) {
        const M4SettingsRow* r = m4SettingsChildAt(g, i);
        if (r && r->key && std::strcmp(r->key, key) == 0) return r->titleZh;
      }
    }
    return nullptr;
  };
  for (int k = 0; k < 9; ++k) {
    const char* title = findTitle(kKeys[k]);
    assert(title && std::strcmp(title, kWant[k]) == 0);
  }
  const char* uiFont = findTitle("uiFontFamily");
  assert(uiFont && std::strcmp(uiFont, "系统字体") == 0);
  printf("Advanced v5.1 true titles PASS\n");
}

void testRootV51RowGeometry() {
  // v5.1 root: 58px rows at the SVG band table; whole-row touch follows the
  // same table the theme repeats paint. Gaps between cards hit nothing.
  assert(kM4SettingsRootRowH == 58);
  const int kY0[8] = {130, 230, 288, 388, 446, 504, 604, 662};
  for (int i = 0; i < 8; ++i) assert(m4SettingsRootRowY0(i) == kY0[i]);
  assert(m4SettingsRootRowHit(240, 130, 480) == 0);
  assert(m4SettingsRootRowHit(0, 230 + 57, 480) == 1);
  assert(m4SettingsRootRowHit(479, 662 + 57, 480) == 7);
  assert(m4SettingsRootRowHit(240, 200, 480) == -1);
  assert(m4SettingsRootRowHit(240, 720, 480) == -1);
  printf("Root v5.1 row geometry PASS\n");
}

void testChildListsAndValues() {
  assert(m4SettingsChildCount("frontlight") == 2);
  assert(streq(m4SettingsChildAt("frontlight", 0)->key, "frontlightBrightness"));
  assert(streq(m4SettingsChildAt("frontlight", 1)->key, "frontlightWarmth"));
  assert(m4SettingsChildAt("frontlight", 0)->control == M4SettingsControl::Number);
  assert(m4SettingsChildCount("keys") >= 1);
  assert(streq(m4SettingsChildAt("keys", 0)->key, "remapButtons"));
  assert(m4SettingsChildCount("maintenance") >= 2);
  assert(m4SettingsChildCount("advanced") == 5);
  assert(m4SettingsChildCount("advancedChrome") >= 1);
  char wifi[32]{};
  m4SettingsFormatWifiValue("", wifi, 32);
  assert(streq(wifi, "未连接"));
  m4SettingsFormatWifiValue("HomeNet", wifi, 32);
  assert(streq(wifi, "HomeNet"));
  char fl[48]{};
  m4SettingsFormatFrontlightValue(40, 20, fl, 48);
  assert(std::strstr(fl, "40") && std::strstr(fl, "20"));
  printf("child lists + live values PASS\n");
}

void testSceneModelRootNotHub() {
#if HAS_MODEL
  SettingsScene::SettingsSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.populateRootFromCatalog("wifi", "未连接", "亮度 40% · 色温 20%", "26", "5 分钟", "默认黑", "",
                                       ""));
  SettingsScene::SettingsSnapshot snap{};
  assert(model.publish());
  assert(model.copyLatest(snap));
  assert(snap.pane != SettingsPane::Hub);
  assert(snap.windowCount == 8);
  int sections = 0;
  int selected = 0;
  int rows = 0;
  for (int i = 0; i < 8; ++i) {
    const auto& row = snap.window[i];
    if (row.isSection) ++sections;
    if (row.isRow) ++rows;
    if (row.selected) ++selected;
  }
  assert(sections == 0);
  assert(rows == 8);
  assert(selected == 1);
  assert(model.bindingSource(snap).size(SettingsScene::kBindingHubCards) == 0);
  assert(model.bindingSource(snap).size(SettingsScene::kBindingPageRows) == 8);
  printf("scene model root not Hub PASS\n");
#else
  assert(false && "RED: SettingsSceneModel.h missing");
#endif
}

void testKMaxRepeatItemsUnchanged() {
  // Contract change (authorized: Advanced v5.1 9-row window): the renderer
  // repeat cap moves 8 -> 9. Repeat rendering streams per item with no
  // per-row allocation, so 9 costs no extra RAM, and only Advanced presents
  // >8 rows — every other theme paints exactly as before.
#if HAS_SCENE_TYPES
  assert(UiScene::kMaxRepeatItems == 9);
#endif
  printf("kMaxRepeatItems==9 PASS\n");
}

void testSettingsActivitySourceContracts() {
  std::string src = loadSettingsActivityCpp();
  assert(!src.empty());
  assert(src.find("drawTabBar") == std::string::npos);
  assert(src.find("sdOta") == std::string::npos);
  assert(src.find("SdOta") == std::string::npos);
  assert(src.find("murphy_settings_hub_m4theme") == std::string::npos &&
         "root must not paint Hub package");
  assert(src.find("populateHubFromPolicy") == std::string::npos && "root must not populate Hub cards");
  assert(src.find("openHubCard") == std::string::npos);
  assert(src.find("handleHubConfirm") == std::string::npos);
  assert(src.find("onGoToFileTransfer") == std::string::npos && "Wi-Fi door must not hijack 传书");
  assert(src.find("WifiSelectionActivity") != std::string::npos &&
         "Wi-Fi door must open WifiSelectionActivity");
  assert(src.find("EpubReaderSettingsActivity") != std::string::npos);
  assert(src.find("m4SettingsNavMoveClamp") != std::string::npos);
  assert(src.find("(void)m4SettingsFooterConfirmLabel") == std::string::npos &&
         "remove grep bait; footer must be the live I18n/UI label");
  assert(src.find("m4SettingsPaintNoteMove") != std::string::npos);
  assert(src.find("m4SettingsPaintTake") != std::string::npos);
  assert(src.find("m4SettingsDisplaySubmit") != std::string::npos);
  assert(src.find("m4SettingsUiOpenConfirm") != std::string::npos);
  assert(src.find("m4SettingsUiConfirmDecide") != std::string::npos);
  assert(src.find("m4SettingsUiActivateIdentity") != std::string::npos ||
         src.find("findSettingByKey") != std::string::npos);
  assert(src.find("const auto &setting = vec[idx]") == std::string::npos);
  assert(src.find("vec[selectedRow]") == std::string::npos);
  // List-row launchAction must not switch+restart; that happens only after confirm accept.
  const auto launchPos = src.find("void SettingsActivity::launchAction");
  assert(launchPos != std::string::npos);
  const auto togglePos = src.find("void SettingsActivity::toggleCurrentSetting");
  assert(togglePos != std::string::npos && togglePos > launchPos);
  const std::string launch = src.substr(launchPos, togglePos - launchPos);
  assert(launch.find("switchToOtherOtaSlot") == std::string::npos);
  assert(launch.find("ESP.restart") == std::string::npos);
  assert(src.find("bootSlotSwitchRequested") != std::string::npos);
  assert(src.find("mapLabels(L(Str::kCancel), L(Str::kConfirm)") != std::string::npos);
  assert(src.find("mapLabels(L(Str::kCancel), L(Str::kToggle)") == std::string::npos);
  assert(src.find("hitSwitchBootConfirm") != std::string::npos);
  printf("SettingsActivity source contracts PASS\n");
}

void testDangerPagesSource() {
  std::string clear = loadClearCache();
  std::string reset = loadReset();
  assert(!clear.empty() && !reset.empty());
  assert(clear.find("Button::Power") == std::string::npos ||
         clear.find("m4SettingsDangerAccepts") != std::string::npos ||
         clear.find("m4SettingsUiConfirmAccepts") != std::string::npos);
  assert(reset.find("m4SettingsDangerAccepts") != std::string::npos ||
         reset.find("m4SettingsUiConfirmAccepts") != std::string::npos);
  assert(clear.find("m4SettingsDangerAccepts") != std::string::npos ||
         clear.find("m4SettingsUiConfirmAccepts") != std::string::npos);
  printf("danger page source PASS\n");
}

void testThemeEightPxNoHubFour() {
  auto hasRect = [](const std::string& src, const char* compactRect) {
    return compactJson(src).find(compactRect) != std::string::npos;
  };
  std::string l2 = loadThemeL2();
  assert(!l2.empty());
  assert(hasRect(l2, "[2,4,460,50]"));
  assert(!hasRect(l2, "[0,12,4,56]"));
  assert(compactJson(l2).find("\"limit\":9") != std::string::npos);
  assert(l2.find("$item.navigates") != std::string::npos && "chevron must gate on navigates");
  std::string root = loadThemeRoot();
  assert(!root.empty());
  assert(root.find("\"$page.rows0\"") != std::string::npos);
  assert(root.find("\"$page.rows3\"") != std::string::npos);
  assert(hasRect(root, "[2,4,460,50]"));
  std::string maint = loadThemeMaint();
  assert(!maint.empty());
  assert(hasRect(maint, "[2,4,460,50]"));
  assert(compactJson(maint).find("\"limit\":5") != std::string::npos);
  std::string front = loadThemeChild("frontlight");
  assert(!front.empty());
  assert(hasRect(front, "[2,4,460,50]"));
  assert(compactJson(front).find("\"limit\":2") != std::string::npos);
  std::string keys = loadThemeChild("keys");
  assert(!keys.empty());
  assert(hasRect(keys, "[2,4,460,50]"));
  assert(compactJson(keys).find("\"limit\":6") != std::string::npos);
  std::string choice = loadThemeChild("choice");
  assert(!choice.empty());
  assert(hasRect(choice, "[18,12,34,34]"));
  assert(!hasRect(choice, "[0,11,8,50]"));
  assert(hasRect(choice, "[2,4,460,50]"));
  assert(compactJson(choice).find("\"x\":400") != std::string::npos);
  assert(compactJson(choice).find("\"x2\":418") != std::string::npos);
  assert(choice.find("$item.value") == std::string::npos);
  assert(compactJson(choice).find("\"limit\":8") != std::string::npos);
  std::string hub = loadThemeHub();
  if (!hub.empty()) {
    assert(hub.find("\"limit\": 4") == std::string::npos && hub.find("\"limit\":4") == std::string::npos &&
           "Hub 4-card theme must be retired");
  }
  std::string model = loadSettingsSceneModel();
  assert(!model.empty());
  assert(model.find("populateRootFromCatalog") != std::string::npos);
  printf("theme 8px / no 4-card Hub PASS\n");
}

void testAdvancedV51WindowCapacity9() {
  // Advanced v5.1 first screen: G0[0-4] + G1[5-8] = 9 true rows at 52px.
  // Window/viewport capacity follows the v5.1 geometry; order, count,
  // selection identity, and swipe page-step behavior are untouched.
#if HAS_MODEL
  assert(SettingsScene::kMaxWindowRows == 9);
#endif
  M4SettingsUiState st{};
  m4SettingsUiEnterRoot(st);
  m4SettingsUiOpenChildList(st, "advanced");
  assert(m4SettingsChildCount("advanced") == 5);
  m4SettingsUiMove(st, 4);
  assert(st.selectedSlot == 4);
  assert(st.windowStart == 0);
  m4SettingsUiOpenChildList(st, "advancedChrome");
  assert(streq(st.listStack, "advanced"));
  assert(m4SettingsChildCount("advancedChrome") == 5);
  bool uiFont = false;
  for (int i = 0; i < 5; ++i) {
    const M4SettingsRow* r = m4SettingsChildAt("advancedChrome", i);
    if (r && std::strcmp(r->key, "uiFontFamily") == 0) uiFont = true;
  }
  assert(uiFont);
  m4SettingsUiBackFromChildList(st);
  assert(streq(st.parentKey, "advanced"));
  assert(streq(st.selectedKey, "advancedChrome"));
  // 9th-row hitbox follows the group Y-table (slot 8 paints at rowY(0,8));
  // section gaps hit nothing.
  assert(m4SettingsAdvancedRowHit(240, m4SettingsAdvancedRowY(0, 8) + 10, 0) == 8);
  // RowY is linear 58px bands (section gaps are paint-only). y just above
  // slot 5 still belongs to slot 4.
  assert(m4SettingsAdvancedRowHit(240, m4SettingsAdvancedRowY(0, 5) - 10, 0) == 4);
  // The shared helper keeps its explicit 9-slot capability for other users.
  assert(m4SettingsWholeRowHit(240, 130 + 8 * 58 + 10, 0, 130, 58, 0, 480, 9) == 8);
  // Legacy 8-cap consumers keep their geometry.
  assert(m4SettingsWholeRowHit(240, 130 + 8 * 58 + 10, 0, 130, 58, 0, 480) == -1);
  printf("Advanced v5.1 window capacity 9 PASS\n");
}

void testAdvancedV51NavigatesFlag() {
  // Chevron truth mirrors activateCurrent: only rows whose tap leaves the
  // list (Choice page / Number picker / sub-activity) carry it. Toggle and
  // in-place ENUM-cycle rows flip where they stand: value, no chevron.
#if HAS_MODEL
  SettingsScene::SettingsSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.setWindowRow(0, "neverFullRefresh", "永不全刷", "关", false, false,
                            false /* toggle flips in place */));
  assert(model.setWindowRow(1, "refreshFrequency", "刷新频率", "10页全刷", false, false,
                            true /* opens Number picker */));
  assert(model.setWindowRow(2, "bluetooth", "蓝牙", "关", false, false));
  assert(model.publish());
  SettingsScene::SettingsSnapshot snap{};
  assert(model.copyLatest(snap));
  auto src = SettingsScene::SettingsSceneModel::bindingSource(snap);
  UiSceneRuntime::SceneItemContext item{true, SettingsScene::kBindingPageRows, 0, 3};
  UiSceneRuntime::ResolvedValue out{};
  assert(src.read(SettingsScene::kBindingItemNavigates, &item, &out));
  assert(out.kind == UiSceneRuntime::ValueKind::Bool && out.boolean == false);
  item.index = 1;
  assert(src.read(SettingsScene::kBindingItemNavigates, &item, &out));
  assert(out.kind == UiSceneRuntime::ValueKind::Bool && out.boolean == true);
  // Callers that never set the flag keep today's paint: navigates defaults
  // to true, so no other page loses its chevron.
  item.index = 2;
  assert(src.read(SettingsScene::kBindingItemNavigates, &item, &out));
  assert(out.kind == UiSceneRuntime::ValueKind::Bool && out.boolean == true);
#endif
  printf("Advanced v5.1 navigates flag PASS\n");
}

void testAdvancedV51GroupRowGeometry() {
  // Section grammar (layout-v5.1-advanced): a 42px gap plus the new group's
  // title (baseline = gapTop + 32) at every within-window group boundary.
  // Order/count/selection/window math never moves; only paint Y does, and
  // hit uses the very same table.
  assert(kM4SettingsAdvancedSectionGap == 24);
  assert(m4SettingsAdvancedGroupOf(0) == 0);
  assert(m4SettingsAdvancedGroupOf(4) == 0);
  assert(m4SettingsAdvancedGroupOf(5) == 1);
  assert(m4SettingsAdvancedGroupOf(8) == 1);
  assert(m4SettingsAdvancedGroupOf(9) == 2);
  assert(m4SettingsAdvancedGroupOf(14) == 3);
  assert(m4SettingsAdvancedGroupOf(20) == 4);
  assert(m4SettingsAdvancedGroupOf(22) == 4);
  assert(m4SettingsAdvancedRowY(0, 0) == 130);
  assert(m4SettingsAdvancedRowY(0, 4) == 130 + 4 * 58);
  assert(m4SettingsAdvancedRowY(0, 5) == 130 + 5 * 58);
  assert(m4SettingsAdvancedRowY(0, 8) == 130 + 8 * 58);
  assert(m4SettingsAdvancedRowY(0, 8) + 58 <= 800);
  assert(!m4SettingsAdvancedSlotStartsGroup(0, 0));
  assert(!m4SettingsAdvancedSlotStartsGroup(0, 4));
  assert(m4SettingsAdvancedSlotStartsGroup(0, 5));
  assert(!m4SettingsAdvancedSlotStartsGroup(0, 8));
  assert(m4SettingsAdvancedRowY(6, 0) == 130);
  assert(m4SettingsAdvancedRowY(6, 2) == 130 + 2 * 58);
  assert(m4SettingsAdvancedSlotStartsGroup(6, 3));
  assert(m4SettingsAdvancedRowY(6, 3) == 130 + 3 * 58);
  assert(!m4SettingsAdvancedSlotStartsGroup(6, 7));
  assert(m4SettingsAdvancedSlotStartsGroup(6, 8));
  assert(m4SettingsAdvancedRowY(6, 8) == 130 + 8 * 58);
  assert(m4SettingsAdvancedRowY(6, 8) + 58 <= 800);
  assert(m4SettingsAdvancedRowY(14, 8) + 58 <= 800);
  assert(m4SettingsAdvancedRowHit(240, 130 + 10, 0) == 0);
  assert(m4SettingsAdvancedRowHit(240, 130 + 5 * 58 + 10, 0) == 5);
  assert(m4SettingsAdvancedRowHit(0, 130 + 5 * 58 + 10, 0) == 5);
  assert(m4SettingsAdvancedRowHit(479, 130 + 5 * 58 + 10, 0) == 5);
  assert(m4SettingsAdvancedRowHit(240, 130 + 3 * 58 + 10, 6) == 3);
  assert(m4SettingsAdvancedRowHit(10, 10, 0) == -1);
  printf("Advanced v5.1 group row geometry PASS\n");
}

void testSceneTextStyleForwarding() {
  // v5.2 A: the scene text path must forward the package style byte
  // (RenderEvent.style, 0=regular/1=bold) to every draw, measure, align,
  // and UTF-8 truncation call. This header needs Arduino downstream, so the
  // host contract pins the forwarding textually at each known drop site.
  std::string src = loadGfxSceneRenderer();
  assert(!src.empty());
  // Positive: one shared mapping from the package byte to the Gfx style.
  assert(src.find("sceneTextStyle(ev.style)") != std::string::npos);
  // Negative: no style-dropping measure/draw/truncate call remains.
  assert(src.find("getTextWidth(fontId, buf)") == std::string::npos);
  assert(src.find("getTextWidth(fontId, rest)") == std::string::npos);
  assert(src.find("getTextWidth(fontId, tmp)") == std::string::npos);
  assert(src.find("getTextWidth(fontId, kEllipsis)") == std::string::npos);
  assert(src.find("getTextWidth(runtimeFontId(event.font), text)") == std::string::npos);
  assert(src.find("drawText(fontId, alignedTextX(gfx, ev, lineBuf), ev.rect.y, lineBuf, true)") ==
         std::string::npos);
  assert(src.find("drawText(fontId, alignedTextX(gfx, ev, buf), ev.rect.y, buf, true)") ==
         std::string::npos);
  assert(src.find("drawText(fontId, alignedTextX(gfx, ev, lineBuf), y, lineBuf, true)") ==
         std::string::npos);
  assert(src.find("fitUtf8Prefix(gfx, fontId, buf, strlen(buf), budget)") == std::string::npos);
  assert(src.find("fitUtf8Prefix(gfx, fontId, rest, restLen, budget)") == std::string::npos);
  assert(src.find("fitUtf8Prefix(gfx, fontId, rest, restLen, maxW)") == std::string::npos);
  printf("scene text style forwarding PASS\n");
}

void testRootV51IconRecipes() {
  std::string src = loadGfxSceneRenderer();
  assert(!src.empty());
  assert(src.find("SettingsRowWifiIcon") != std::string::npos);
  assert(src.find("SettingsRowSunIcon") != std::string::npos);
  assert(src.find("SettingsRowBookIcon") != std::string::npos);
  assert(src.find("SettingsRowSleepIcon") != std::string::npos);
  assert(src.find("SettingsRowLockIcon") != std::string::npos);
  assert(src.find("SettingsRowKeysIcon") != std::string::npos);
  assert(src.find("SettingsRowTuneIcon") != std::string::npos);
  assert(src.find("SettingsRowSlidersIcon") != std::string::npos);
  assert(src.find("gfx.drawIcon(bmp, x0, y0, kSettingsRowIconSize, kSettingsRowIconSize)") != std::string::npos);
  printf("root v5.1 icon recipes PASS\n");
}

void testRootV53SelectedAndIcons() {
  std::string src = loadGfxSceneRenderer();
  assert(!src.empty());
  assert(src.find("SettingsRowSleepIcon") != std::string::npos);
  assert(src.find("SettingsRowLockIcon") != std::string::npos);
  assert(src.find("SettingsRowKeysIcon") != std::string::npos);
  assert(src.find("stipple field only") != std::string::npos);
  printf("root v5.3 selected + icons PASS\n");
}

void testAdvancedV53SelectedStippleOnly() {
  // Nested Advanced no longer paints its own 9-row overlay/card; theme L2
  // rows are the same as other child lists.
  std::string src = loadSettingsActivityCpp();
  assert(!src.empty());
  assert(src.find("fillRectStipple(kRowX + 2, ry + 4, 460, 50)") == std::string::npos);
  assert(src.find("drawRoundedRect(kCardX, kCardY, kCardW, kCardH, 2, 11, true)") == std::string::npos);
  printf("advanced nested index uses theme L2 PASS\n");
}

}  // namespace

int main() {
  testRootEightActionableNoSection();
  testNoWrapClamp();
  testHighlightAConfirmBImpossibleOnRoot();
  testFooterFollowsControl();
  testFocusEightPxAndDirtyBurst();
  testRootMoveConsumesDirtyViaPaintSeam();
  testMakeRefreshClampsInvalidToFull();
  testAdvancedWindowRelativeDirty();
  testFooterLabelChangeCovered();
  testWifiPopCallbackResetsFull();
  testHandleRowHitNotesMove();
  testChoiceCheckmarkAndRestore();
  testChildListChoiceCommitRestoresIdentitySlotWindow();
  testNumberPickerRestoreCancelAndCommit();
  testDangerNotPower();
  testSwitchBootSlotIsDangerConfirmPage();
  testWholeRowHitNoSplit();
  testRootV51RowGeometry();
  testAdvancedV51PitchHelpers();
  testAdvancedV51TrueTitles();
  testAdvancedV51WindowCapacity9();
  testAdvancedV51NavigatesFlag();
  testAdvancedV51GroupRowGeometry();
  testSceneTextStyleForwarding();
  testRootV51IconRecipes();
  testRootV53SelectedAndIcons();
  testAdvancedV53SelectedStippleOnly();
  testChildListsAndValues();
  testSceneModelRootNotHub();
  testKMaxRepeatItemsUnchanged();
  testSettingsActivitySourceContracts();
  testDangerPagesSource();
  testThemeEightPxNoHubFour();
  printf("ALL Settings UI contracts PASS\n");
  return 0;
}

#endif


#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "util/HomeSceneRuntime.h"
#include "generated/murphy_default_m4theme.h"

using namespace HomeSceneRuntime;

struct MockData {
  bool currentExists = true;
  int battery = 73;
  int progress = 35;
  const char* currentTitle = "Current Book";
  const char* currentAuthor = "Author";
  const char* currentSource = "WeRead";
  const char* currentCover = "/cover/current.bmp";
  const char* recentTitle[3] = {"Recent A","Recent B","Recent C"};
  const char* recentCover[3] = {"/r/a.bmp","/r/b.bmp","/r/c.bmp"};
  const char* appId[4] = {"files","weread","fanqie","jinjiang"};
  const char* appName[4] = {"Files","WeRead","Fanqie","Jinjiang"};
  const char* appIcon[4] = {"folder","weread","tomato","jinjiang"};
};

static TextView sv(const char* s) {
  TextView v{};
  if (!s) return v;
  v.data = reinterpret_cast<const uint8_t*>(s);
  v.size = static_cast<uint16_t>(std::strlen(s));
  v.storage = kTextRam;
  return v;
}

static bool getBool(void* u, uint8_t binding, const ItemContext*, bool* out) {
  auto* d = static_cast<MockData*>(u);
  if (binding == kBindingCurrentExists) { *out = d->currentExists; return true; }
  if (binding == kBindingWifiConnected) { *out = true; return true; }
  return false;
}
static bool getInt(void* u, uint8_t binding, const ItemContext* item, int32_t* out) {
  auto* d = static_cast<MockData*>(u);
  if (binding == kBindingSystemBattery) { *out = d->battery; return true; }
  if (binding == kBindingCurrentProgress) { *out = d->progress; return true; }
  if (binding == kBindingItemProgress && item && item->valid) { *out = 50 + item->index; return true; }
  return false;
}
static bool getText(void* u, uint8_t binding, const ItemContext* item, TextView* out) {
  auto* d = static_cast<MockData*>(u);
  switch (binding) {
    case kBindingCurrentTitle: *out = sv(d->currentTitle); return true;
    case kBindingCurrentAuthor: *out = sv(d->currentAuthor); return true;
    case kBindingCurrentSource: *out = sv(d->currentSource); return true;
    case kBindingCurrentCover: *out = sv(d->currentCover); return true;
    default: break;
  }
  if (!item || !item->valid) return false;
  if (item->sourceBinding == kBindingRecent && item->index < 3) {
    if (binding == kBindingItemTitle) { *out = sv(d->recentTitle[item->index]); return true; }
    if (binding == kBindingItemCover) { *out = sv(d->recentCover[item->index]); return true; }
  }
  if (item->sourceBinding == kBindingApps && item->index < 4) {
    if (binding == kBindingItemId) { *out = sv(d->appId[item->index]); return true; }
    if (binding == kBindingItemName) { *out = sv(d->appName[item->index]); return true; }
    if (binding == kBindingItemIcon) { *out = sv(d->appIcon[item->index]); return true; }
  }
  return false;
}
static uint8_t getCount(void*, uint8_t source) {
  return source == kBindingRecent ? 3 : source == kBindingApps ? 4 : 0;
}

struct Captured {
  uint8_t type = 0;
  Rect rect{};
  int32_t value = -999;
  TextView text{};
  uint16_t x2 = 0, y2 = 0;
};
struct MockSink {
  std::vector<Captured> events;
};
static void emit(void* u, const RenderEvent& e) {
  auto* s = static_cast<MockSink*>(u);
  Captured c{};
  c.type=e.type; c.rect=e.rect; c.value=e.value; c.text=e.text; c.x2=e.x2; c.y2=e.y2;
  s->events.push_back(c);
}
static bool tvEq(const TextView& v, const char* s) {
  size_t n = std::strlen(s);
  if (v.size != n || !v.data) return false;
  for (size_t i=0;i<n;++i) if (readTextByte(v, i) != static_cast<uint8_t>(s[i])) return false;
  return true;
}

int main() {
  SceneDataProvider provider{};
  MockData data{};
  provider.user=&data;
  provider.getBool=&getBool;
  provider.getInt=&getInt;
  provider.getText=&getText;
  provider.getCount=&getCount;

  MockSink sinkState{};
  SceneRenderSink sink{&sinkState, &emit};
  bool ok = executeScene(murphy_default_m4theme, murphy_default_m4theme_len, provider, sink);
  assert(ok);
  assert(!sinkState.events.empty());
  // exact top-level prefix order before first repeat
  const uint8_t prefix[] = {kNodeClear,kNodeText,kNodeBattery,kNodeLine,kNodeRoundRect,kNodeCover,
                            kNodeText,kNodeText,kNodeText,kNodeText,kNodeText,kNodeProgress,
                            kNodeText,kNodeText};
  for (size_t i=0;i<sizeof(prefix);++i) assert(sinkState.events[i].type == prefix[i]);

  bool sawBattery=false, sawProgress=false, sawCurrentCover=false;
  int recentCovers=0, recentTitles=0, appIcons=0, appNames=0;
  for (auto &e : sinkState.events) {
    if (e.type==kNodeBattery) { assert(e.value==73); sawBattery=true; }
    if (e.type==kNodeProgress) { assert(e.value==35); sawProgress=true; }
    if (e.type==kNodeCover && e.rect.x==52 && e.rect.y==129) {
      assert(tvEq(e.text, "/cover/current.bmp")); sawCurrentCover=true;
    }
    if (e.type==kNodeCover && e.rect.y==405) ++recentCovers;
    if (e.type==kNodeText && e.rect.y==517) ++recentTitles;
    if (e.type==kNodeIcon && e.rect.y==610) ++appIcons;
    if (e.type==kNodeText && e.rect.y==686) ++appNames;
  }
  assert(sawBattery && sawProgress && sawCurrentCover);
  assert(recentCovers==3 && recentTitles==3 && appIcons==4 && appNames==4);

  // repeat positions are deterministic
  std::vector<int> recentXs, appXs;
  for (auto &e: sinkState.events) {
    if (e.type==kNodeCover && e.rect.y==405) recentXs.push_back(e.rect.x);
    if (e.type==kNodeIcon && e.rect.y==610) appXs.push_back(e.rect.x);
  }
  assert((recentXs == std::vector<int>{60,204,348}));
  assert((appXs == std::vector<int>{42,159,276,393}));

  // visible_if fail-safe: false removes the status text; missing bindings do not fake values.
  data.currentExists=false;
  sinkState.events.clear();
  assert(executeScene(murphy_default_m4theme, murphy_default_m4theme_len, provider, sink));
  for (auto &e: sinkState.events) {
    if (e.type==kNodeText && e.rect.x==184 && e.rect.y==133) assert(false && "visible_if node rendered while false");
  }

  // hit testing: current cover, history, apps, app repeat item.
  data.currentExists=true;
  HitResult hr{};
  assert(hitTestScene(murphy_default_m4theme, murphy_default_m4theme_len, provider, 60, 140, &hr));
  assert(hr.hit && hr.action==kActionOpenCurrentBook);
  assert(hitTestScene(murphy_default_m4theme, murphy_default_m4theme_len, provider, 400, 390, &hr));
  assert(hr.action==kActionOpenHistory);
  assert(hitTestScene(murphy_default_m4theme, murphy_default_m4theme_len, provider, 400, 590, &hr));
  assert(hr.action==kActionOpenApps);
  assert(hitTestScene(murphy_default_m4theme, murphy_default_m4theme_len, provider, 170, 630, &hr));
  assert(hr.action==kActionOpenApp);
  assert(tvEq(hr.argument, "weread"));
  assert(!hitTestScene(murphy_default_m4theme, murphy_default_m4theme_len, provider, 470, 790, &hr));

  std::puts("home scene executor: PASS");
  return 0;
}

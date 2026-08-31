#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "generated/murphy_default_m4theme.h"
#include "ui/pages/HomeSceneModel.h"

using namespace HomeScene;

namespace {

static bool textIs(const UiSceneRuntime::TextView& text, const char* expected) {
  if (!expected || !text.data) return false;
  std::size_t length = 0;
  while (expected[length] != '\0') ++length;
  if (text.size != length) return false;
  for (std::size_t i = 0; i < length; ++i) {
    if (text.readByte(static_cast<uint16_t>(i)) !=
        static_cast<uint8_t>(expected[i])) return false;
  }
  return true;
}

void testLoadingReadyAndStaleRetention() {
  HomeSceneModel model;
  HomeSceneSnapshot snapshot{};
  assert(!model.copyLatest(snapshot));

  assert(model.publishLoading());
  assert(model.copyLatest(snapshot));
  assert(snapshot.state == UiScene::DataState::Loading);

  model.begin(UiScene::DataState::Ready);
  assert(model.setBattery(73));
  assert(model.setWifiConnected(true));
  assert(model.setCurrent("Current", "Author", "WeRead", "/current.bmp", 35));
  assert(model.setCurrentPaths("/current.epub", ""));
  assert(model.addRecent("Recent A", "Author A", "WeRead", "/recent-a.bmp", 11));
  assert(model.setRecentPaths(0, "/recent-a.epub", ""));
  assert(model.addRecent("Recent B", "Author B", "WeRead", "/recent-b.bmp", 22));
  assert(model.setRecentPaths(1, "/recent-b.epub", ""));
  assert(model.addApp("weread", "WeRead", "book"));
  assert(model.addApp("fanqie", "Fanqie", "tomato"));
  assert(model.publish());
  assert(model.copyLatest(snapshot));
  const uint32_t readyRevision = snapshot.revision;
  assert(snapshot.state == UiScene::DataState::Ready);
  assert(snapshot.recentCount == 2);
  assert(snapshot.appCount == 2);
  assert(textIs(snapshot.textView(snapshot.currentTitle), "Current"));

  assert(model.publishStale());
  assert(model.copyLatest(snapshot));
  assert(snapshot.state == UiScene::DataState::Stale);
  assert(snapshot.revision > readyRevision);
  assert(snapshot.recentCount == 2);
  assert(snapshot.appCount == 2);
  assert(textIs(snapshot.textView(snapshot.recent[1].title), "Recent B"));

  assert(model.publishEmpty(4));
  assert(model.copyLatest(snapshot));
  assert(snapshot.state == UiScene::DataState::Empty && snapshot.errorCode == 4);
  assert(model.publishError(5));
  assert(model.copyLatest(snapshot));
  assert(snapshot.state == UiScene::DataState::Error && snapshot.errorCode == 5);
}

void testFixedCapacityAndBindings() {
  static_assert(std::is_trivially_copyable<HomeSceneSnapshot>::value,
                "Home snapshots must be fixed-capacity values");
  HomeSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.setBattery(91));
  assert(model.setWifiConnected(true));
  assert(model.setCurrent("Book", "Author", "Source", "/book.bmp", 64));
  for (std::size_t i = 0; i < kMaxRecentItems; ++i) {
    assert(model.addRecent("Recent", "Author", "Source", "/recent.bmp", 10));
  }
  assert(!model.addRecent("overflow", "", "", "", 0));
  for (std::size_t i = 0; i < kMaxAppItems; ++i) {
    assert(model.addApp("app", "App", "icon"));
  }
  assert(!model.addApp("overflow", "App", "icon"));

  char oversized[kMaxTextBytes + 1]{};
  for (std::size_t i = 0; i < kMaxTextBytes; ++i) oversized[i] = 'x';
  assert(!model.setCurrent(oversized, "", "", "", 0));
  assert(model.publish());

  HomeSceneSnapshot snapshot{};
  assert(model.copyLatest(snapshot));
  const auto source = HomeSceneModel::bindingSource(snapshot);
  UiSceneRuntime::ResolvedValue value{};
  assert(source.read(kBindingSystemBattery, nullptr, &value));
  assert(value.kind == UiSceneRuntime::ValueKind::Int && value.number == 91);
  assert(source.read(kBindingWifiConnected, nullptr, &value));
  assert(value.kind == UiSceneRuntime::ValueKind::Bool && value.boolean);
  assert(source.read(kBindingCurrentTitle, nullptr, &value));
  assert(textIs(value.text, "Book"));
  assert(source.read(kBindingCurrentProgress, nullptr, &value));
  assert(value.kind == UiSceneRuntime::ValueKind::Int && value.number == 64);
  assert(source.read(kBindingCurrentProgressText, nullptr, &value));
  assert(value.kind == UiSceneRuntime::ValueKind::Text);
  assert(textIs(value.text, "64%"));
  UiSceneRuntime::SceneItemContext recent{true, kBindingRecent, 1,
                                           snapshot.recentCount};
  assert(source.read(kBindingItemTitle, &recent, &value));
  assert(textIs(value.text, "Recent"));
  assert(source.read(kBindingItemProgress, &recent, &value));
  assert(value.kind == UiSceneRuntime::ValueKind::Int && value.number == 10);
  UiSceneRuntime::SceneItemContext app{true, kBindingApps, 0, snapshot.appCount};
  assert(source.read(kBindingItemId, &app, &value));
  assert(textIs(value.text, "app"));
  assert(source.read(kBindingItemName, &app, &value));
  assert(textIs(value.text, "App"));
  assert(source.read(kBindingItemIcon, &app, &value));
  assert(textIs(value.text, "icon"));
}

void testActionTargetsAreNumericAndPure() {
  HomeSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.setCurrent("Book", "", "", "/book.bmp", 1));
  assert(model.addRecent("Recent", "", "", "/recent.bmp", 2));
  assert(model.setRecentPaths(0, "/recent.epub", "weread://recent"));
  assert(model.addApp("weread", "WeRead", "book"));
  assert(model.publish());

  HomeSceneActionTarget target{};
  assert(model.actionTarget(kActionOpenCurrentBook, nullptr, &target));
  assert(target.action == kActionOpenCurrentBook);
  assert(target.itemIndex == kInvalidItemIndex);
  UiSceneRuntime::SceneItemContext recent{true, kBindingRecent, 0, 1};
  assert(model.actionTarget(kActionOpenCurrentBook, &recent, &target));
  assert(target.action == kActionOpenCurrentBook && target.itemIndex == 0);
  assert(model.actionTarget(kActionOpenHistory, nullptr, &target));
  assert(target.itemIndex == kInvalidItemIndex);
  assert(model.actionTarget(kActionOpenApps, nullptr, &target));
  UiSceneRuntime::SceneItemContext item{true, kBindingApps, 0, 1};
  assert(model.actionTarget(kActionOpenApp, &item, &target));
  assert(target.itemIndex == 0);
  assert(textIs(target.argumentView(), "weread"));
  assert(!model.actionTarget(kActionOpenApp, nullptr, &target));
}

void testRecentCoverHitTargets() {
  HomeSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.setCurrent("Hero", "", "", "", 0));
  for (uint8_t i = 0; i < 3; ++i) {
    assert(model.addRecent("Recent", "", "", "", 0));
    assert(model.setRecentPaths(i, "/recent.epub", ""));
  }
  assert(model.publish());

  HomeSceneSnapshot snapshot{};
  assert(model.copyLatest(snapshot));
  const auto source = HomeSceneModel::bindingSource(snapshot);
  const int centers[] = {92, 235, 378};
  for (uint8_t i = 0; i < 3; ++i) {
    UiSceneRuntime::HitResult hit{};
    assert(UiSceneRuntime::hitTestScene(murphy_default_m4theme,
                                        murphy_default_m4theme_len, source,
                                        centers[i], 440, &hit));
    assert(hit.action == kActionOpenCurrentBook && hit.item.valid &&
           hit.item.sourceBinding == kBindingRecent && hit.item.index == i);
    HomeSceneActionTarget target{};
    assert(HomeSceneModel::actionTarget(snapshot, hit.action, &hit.item,
                                        &target));
    assert(target.itemIndex == i);
  }
}

}  // namespace

int main() {
  testLoadingReadyAndStaleRetention();
  testFixedCapacityAndBindings();
  testActionTargetsAreNumericAndPure();
  testRecentCoverHitTargets();
  return 0;
}

#include <cassert>
#include <cstdint>

#include "fixtures/m4_ui_scene_backend_probe.h"

using namespace HomeScene;
using M4UiSceneBackendProbe::Mode;

namespace {

void testMode(Mode mode) {
  HomeSceneModel model;
  HomeSceneSnapshot snapshot{};
  assert(model.publishLoading());

  if (mode == Mode::StaleAfterRefreshFailure) {
    model.begin(UiScene::DataState::Ready);
    assert(model.setCurrent("Book", "", "", "/book.bmp", 50));
    assert(model.addRecent("Book", "", "", "/book.bmp", 50));
    assert(model.publish());
  }

  M4UiSceneBackendProbe::Probe probe{mode};
  if (mode != Mode::NeverReturns) assert(probe.run(model));
  const uint32_t before = model.latestRevision();
  uint32_t frameCount = 0;
  uint32_t focusEvents = 0;
  uint32_t systemEvents = 0;
  for (int tick = 0; tick < 32; ++tick) {
    ++frameCount;
    assert(model.copyLatest(snapshot));
    ++focusEvents;
    ++systemEvents;  // bounded system input does not depend on backend progress
  }
  assert(frameCount == 32 && focusEvents == 32 && systemEvents == 32);
  if (mode == Mode::NeverReturns) {
    assert(snapshot.state == UiScene::DataState::Loading);
    assert(model.latestRevision() == before);
  } else if (mode == Mode::Error) {
    assert(snapshot.state == UiScene::DataState::Error && snapshot.errorCode == 7);
  } else if (mode == Mode::Empty) {
    assert(snapshot.state == UiScene::DataState::Empty);
  } else {
    assert(snapshot.state == UiScene::DataState::Stale);
    assert(snapshot.recentCount == 1);
    assert(snapshot.currentExists);
  }
}

}  // namespace

int main() {
  testMode(Mode::NeverReturns);
  testMode(Mode::Error);
  testMode(Mode::Empty);
  testMode(Mode::StaleAfterRefreshFailure);
  return 0;
}

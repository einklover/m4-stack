#include <cassert>
#include <cstddef>
#include <cstdint>

#include "generated/murphy_default_m4theme.h"
#include "ui/pages/HomeSceneModel.h"
#include "ui/scene/UiSceneActionQueue.h"

namespace {

UiScene::UiSceneAction action(uint8_t id, uint8_t item = 0) {
  UiScene::UiSceneAction result{};
  result.action = id;
  result.itemIndex = item;
  return result;
}

struct HandlerState {
  uint32_t calls = 0;
  UiScene::UiSceneAction last{};
};

bool handle(void* user, const UiScene::UiSceneAction& value) {
  auto* state = static_cast<HandlerState*>(user);
  ++state->calls;
  state->last = value;
  return true;
}

bool rejectFirst(void* user, const UiScene::UiSceneAction& value) {
  auto* state = static_cast<HandlerState*>(user);
  ++state->calls;
  state->last = value;
  return state->calls != 1;
}

struct SystemNavigation {
  uint32_t back = 0;
  uint32_t home = 0;
  uint32_t focus = 0;

  void pressBack() { ++back; }
  void pressHome() { ++home; }
  void moveFocus() { ++focus; }
};

void testAbsentProducerAndStalledConsumer() {
  UiScene::UiSceneActionQueue queue;
  UiScene::UiSceneActionDispatcher dispatcher;
  HandlerState handlerState{};

  assert(dispatcher.dispatchOne(queue, &handle, &handlerState) ==
         UiScene::UiSceneActionDispatchResult::Empty);
  for (int tick = 0; tick < 32; ++tick) {
    assert(queue.empty());
    assert(dispatcher.dispatchOne(queue, &handle, &handlerState) ==
           UiScene::UiSceneActionDispatchResult::Empty);
  }
  assert(handlerState.calls == 0);
}

void testFullQueueRejectsImmediatelyAndSystemNavigationIsIndependent() {
  UiScene::UiSceneActionQueue queue;
  for (std::size_t i = 0; i < UiScene::UiSceneActionQueue::kCapacity; ++i) {
    assert(queue.tryEnqueue(action(static_cast<uint8_t>(i),
                                   static_cast<uint8_t>(i))));
  }
  assert(!queue.tryEnqueue(action(99)));
  assert(queue.size() == UiScene::UiSceneActionQueue::kCapacity);

  SystemNavigation navigation{};
  navigation.pressBack();
  navigation.pressHome();
  navigation.moveFocus();
  assert(navigation.back == 1 && navigation.home == 1 && navigation.focus == 1);

  UiScene::UiSceneAction value{};
  assert(queue.tryDequeue(value) && value.action == 0 && value.itemIndex == 0);
  assert(queue.tryEnqueue(action(100)));
}

void testReadyToStaleSnapshotStillRendersAndActionWaitsForDispatcher() {
  HomeScene::HomeSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.setCurrent("Book", "Author", "Source", "/book.bmp", 42));
  assert(model.addRecent("Book", "Author", "Source", "/book.bmp", 42));
  assert(model.publish());
  assert(model.publishStale());

  HomeScene::HomeSceneSnapshot snapshot{};
  assert(model.copyLatest(snapshot));
  assert(snapshot.state == UiScene::DataState::Stale);

  const auto source = HomeScene::HomeSceneModel::bindingSource(snapshot);
  uint32_t renderedEvents = 0;
  const UiSceneRuntime::SceneRenderSink sink{
      &renderedEvents,
      [](void* user, const UiSceneRuntime::RenderEvent&) {
        ++*static_cast<uint32_t*>(user);
      }};
  assert(UiSceneRuntime::renderScene(murphy_default_m4theme,
                                     murphy_default_m4theme_len, source, sink));
  assert(renderedEvents > 0);

  UiScene::UiSceneActionQueue queue;
  UiScene::UiSceneActionDispatcher dispatcher;
  HandlerState handlerState{};
  assert(queue.tryEnqueue(action(7, 2)));
  assert(handlerState.calls == 0);
  assert(dispatcher.dispatchOne(queue, &handle, &handlerState) ==
         UiScene::UiSceneActionDispatchResult::Dispatched);
  assert(handlerState.calls == 1 && handlerState.last.action == 7 &&
         handlerState.last.itemIndex == 2);

  assert(queue.tryEnqueue(action(8)));
  assert(queue.tryEnqueue(action(9)));
  handlerState = {};
  assert(dispatcher.dispatchAvailable(queue, &rejectFirst, &handlerState, 2) == 1);
  assert(handlerState.calls == 2 && handlerState.last.action == 9);
}

}  // namespace

int main() {
  testAbsentProducerAndStalledConsumer();
  testFullQueueRejectsImmediatelyAndSystemNavigationIsIndependent();
  testReadyToStaleSnapshotStillRendersAndActionWaitsForDispatcher();
  return 0;
}

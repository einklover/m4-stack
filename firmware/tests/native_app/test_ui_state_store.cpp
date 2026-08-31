#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <type_traits>

#include "ui/scene/UiStateStore.h"
#include "ui/scene/UiSceneTypes.h"

namespace {

using UiScene::DataState;

struct Snapshot {
  DataState state;
  std::uint32_t value;
};

static_assert(std::is_trivially_copyable<Snapshot>::value, "test snapshot must be fixed-capacity");
static_assert(UiStateStore<Snapshot>::kSlotCount == 3, "the store must have exactly three slots");

void testInitialState() {
  UiStateStore<Snapshot> store({DataState::Empty, 7});
  auto snapshot = store.acquire();

  assert(snapshot.valid());
  assert(snapshot.value().state == DataState::Empty);
  assert(snapshot.value().value == 7);
  assert(snapshot.generation() == 0);
}

void testEveryDataStateAndGeneration() {
  UiStateStore<Snapshot> store({DataState::Empty, 0});
  const DataState states[5] = {
      DataState::Loading,
      DataState::Ready,
      DataState::Stale,
      DataState::Empty,
      DataState::Error,
  };

  std::uint64_t expectedGeneration = 0;
  for (const DataState state : states) {
    ++expectedGeneration;
    assert(store.tryPublish({state, static_cast<std::uint32_t>(expectedGeneration)}));
    auto snapshot = store.acquire();
    assert(snapshot.valid());
    assert(snapshot.value().state == state);
    assert(snapshot.value().value == expectedGeneration);
    assert(snapshot.generation() == expectedGeneration);
  }
}

void testPinnedOldAndCurrentSnapshotsRemainStable() {
  UiStateStore<Snapshot> store({DataState::Loading, 1});
  auto oldest = store.acquire();

  assert(store.tryPublish({DataState::Ready, 2}));
  auto older = store.acquire();

  assert(store.tryPublish({DataState::Stale, 3}));
  auto current = store.acquire();

  assert(oldest.valid());
  assert(older.valid());
  assert(current.valid());
  assert(oldest.value().state == DataState::Loading);
  assert(oldest.value().value == 1);
  assert(oldest.generation() == 0);
  assert(older.value().state == DataState::Ready);
  assert(older.value().value == 2);
  assert(older.generation() == 1);
  assert(current.value().state == DataState::Stale);
  assert(current.value().value == 3);
  assert(current.generation() == 2);

  // Both non-current slots are pinned, so the current snapshot is unchanged
  // and publication fails without waiting for either reader.
  assert(!store.tryPublish({DataState::Error, 4}));
  assert(current.value().state == DataState::Stale);
  assert(current.value().value == 3);
}

void testAllSlotsPinnedFailsThenReleaseAllowsPublish() {
  UiStateStore<Snapshot> store({DataState::Loading, 0});
  auto oldest = store.acquire();

  assert(store.tryPublish({DataState::Ready, 1}));
  auto older = store.acquire();

  assert(store.tryPublish({DataState::Stale, 2}));
  auto current = store.acquire();

  assert(oldest.valid());
  assert(older.valid());
  assert(current.valid());
  assert(!store.tryPublish({DataState::Error, 3}));
  assert(current.value().state == DataState::Stale);
  assert(current.value().value == 2);
  assert(current.generation() == 2);

  oldest.release();
  assert(store.tryPublish({DataState::Error, 3}));
  auto latest = store.acquire();
  assert(latest.valid());
  assert(latest.value().state == DataState::Error);
  assert(latest.value().value == 3);
  assert(latest.generation() == 3);
}

struct SentinelSnapshot {
  std::uint64_t sequence;
  std::uint64_t mirror;
  std::uint64_t fields[64];
};

SentinelSnapshot makeSentinel(std::uint64_t sequence) {
  SentinelSnapshot result{};
  result.sequence = sequence;
  result.mirror = ~sequence;
  for (std::size_t i = 0; i < 64; ++i) {
    result.fields[i] = sequence ^ static_cast<std::uint64_t>(i);
  }
  return result;
}

void assertComplete(const SentinelSnapshot& snapshot) {
  assert(snapshot.mirror == ~snapshot.sequence);
  for (std::size_t i = 0; i < 64; ++i) {
    assert(snapshot.fields[i] == (snapshot.sequence ^ static_cast<std::uint64_t>(i)));
  }
}

struct WriterContext {
  UiStateStore<SentinelSnapshot>* store;
  std::atomic<bool>* done;
  std::uint64_t publicationCount;
};

void* publishSentinels(void* argument) {
  WriterContext* context = static_cast<WriterContext*>(argument);
  for (std::uint64_t sequence = 1; sequence <= context->publicationCount; ++sequence) {
      const SentinelSnapshot next = makeSentinel(sequence);
      while (!context->store->tryPublish(next)) {
        sched_yield();
      }
  }
  context->done->store(true, std::memory_order_release);
  return nullptr;
}

struct ReaderContext {
  UiStateStore<SentinelSnapshot>* store;
  std::atomic<bool>* done;
};

void* readSentinels(void* argument) {
  ReaderContext* context = static_cast<ReaderContext*>(argument);
  while (!context->done->load(std::memory_order_acquire)) {
    auto snapshot = context->store->acquire();
    assert(snapshot.valid());
    assertComplete(snapshot.value());
  }
  return nullptr;
}

void testPublicationIsCompleteBeforeVisible() {
  constexpr std::uint64_t kPublicationCount = 20000;
  UiStateStore<SentinelSnapshot> store(makeSentinel(0));
  std::atomic<bool> writerDone{false};
  WriterContext context{&store, &writerDone, kPublicationCount};
  ReaderContext readerContext{&store, &writerDone};
  pthread_t writer;
  pthread_t readers[2];
  assert(pthread_create(&writer, nullptr, publishSentinels, &context) == 0);
  assert(pthread_create(&readers[0], nullptr, readSentinels, &readerContext) == 0);
  assert(pthread_create(&readers[1], nullptr, readSentinels, &readerContext) == 0);

  while (!writerDone.load(std::memory_order_acquire)) {
    auto snapshot = store.acquire();
    assert(snapshot.valid());
    assertComplete(snapshot.value());
  }

  assert(pthread_join(writer, nullptr) == 0);
  assert(pthread_join(readers[0], nullptr) == 0);
  assert(pthread_join(readers[1], nullptr) == 0);
  auto finalSnapshot = store.acquire();
  assert(finalSnapshot.valid());
  assert(finalSnapshot.value().sequence == kPublicationCount);
  assert(finalSnapshot.generation() == kPublicationCount);
  assertComplete(finalSnapshot.value());
}

void testSingleWriterContract() {
  static_assert(!std::is_copy_constructible<UiStateStore<Snapshot>>::value,
                "the store is owned by one producer");
  static_assert(!std::is_copy_assignable<UiStateStore<Snapshot>>::value,
                "the store is owned by one producer");
}

}  // namespace

int main() {
  testInitialState();
  testEveryDataStateAndGeneration();
  testPinnedOldAndCurrentSnapshotsRemainStable();
  testAllSlotsPinnedFailsThenReleaseAllowsPublish();
  testPublicationIsCompleteBeforeVisible();
  testSingleWriterContract();
  return 0;
}

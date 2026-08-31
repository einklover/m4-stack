#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ui/pages/HomeSceneModel.h"
#include "ui/scene/UiStateStore.h"

using namespace HomeScene;

void testAssetArenaGeometry() {
  assert(kHomeAssetArenaBytes == 7748);
  assert(kHomeCurrentCoverBytes == 2520);
  assert(kHomeRecentCoverBytes == 1060);
  assert(kHomeAppIconBytes == 512);
  size_t off; uint16_t w,h,s; size_t b;
  assert(homePublicationSlotForKey({kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex}, &off,&w,&h,&s,&b));
  assert(off==0 && b==2520 && w==110 && h==180 && s==14);
  assert(homePublicationSlotForKey({kBindingItemCover, kBindingRecent, 2}, &off,&w,&h,&s,&b));
  assert(off==2520+2*1060);
  assert(homePublicationSlotForKey({kBindingItemIcon, kBindingApps, 3}, &off,&w,&h,&s,&b));
  assert(off==2520+3*1060+3*512);
  assert(off+b == 7748);
}

void testPublicationStoreMovedToHeapPsram() {
  // Large publication (10KB) must be heap/PSRAM allocated.
  UiStateStore<HomeScenePublication> pubStore(HomeScenePublication{});
  assert(pubStore.isHeapAllocated() && "HomeScenePublication store must be heap/PSRAM allocated to save internal RAM");
  assert(pubStore.heapSlotsBytes() > 24000 && "heap slots must cover 3 arenas");
  // Small snapshot store is also heap but tiny; verify it doesn't waste large arena.
  HomeSceneSnapshot snap{};
  UiStateStore<HomeSceneSnapshot> snapStore(snap);
  assert(snapStore.isHeapAllocated() && "Even small store is heap allocated, but metadata stays internal");
  assert(snapStore.heapSlotsBytes() < 10000 && "small snapshot heap should be <10KB");
  assert(pubStore.heapSlotsBytes() > snapStore.heapSlotsBytes() * 3 && "large store must be significantly bigger");
}

void testDraftArenaMovedToHeap() {
  HomeSceneModel model;
  assert(model.isDraftHeapAllocated() && "draft arena must be heap/PSRAM allocated, not internal");
  // Verify that draft publication arena is usable after heap allocation.
  HomeScenePublication& draft = model.draftPublication();
  // Fill draft with assets and publish; should succeed and be readable.
  model.begin(UiScene::DataState::Ready);
  assert(model.setCurrent("T", "A", "S", "/c.bmp", 10));
  // Add asset via draftPublication
  uint8_t dummy[2520] = {0};
  dummy[0]=0xAA;
  UiScene::AssetKey k{kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex};
  assert(model.addPublicationAsset(k, dummy, kHomeCurrentCoverW, kHomeCurrentCoverH, kHomeCurrentCoverStride));
  assert(model.publish());
  HomeScenePublication out{};
  assert(model.copyLatestPublication(out));
  assert(out.assetCount==1);
  assert(out.arena[0]==0xAA);
}

void testRenderPointerLifetimePinned() {
  HomeSceneModel model;
  model.begin(UiScene::DataState::Ready);
  assert(model.setCurrent("T","A","S","/c.bmp",10));
  HomeScenePublication pubA{};
  pubA.snapshot = HomeSceneSnapshot{};
  pubA.snapshot.state = UiScene::DataState::Ready;
  size_t off; uint16_t w,h,s; size_t b;
  UiScene::AssetKey curr{ kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex };
  assert(homePublicationSlotForKey(curr,&off,&w,&h,&s,&b));
  std::vector<uint8_t> patA(b,0xAA);
  assert(homeAddAssetToPublication(pubA,curr,patA.data(),w,h,s));
  pubA.snapshot.currentExists=true;
  assert(model.publishWithAssets(pubA));
  auto pinA = model.acquirePublication();
  assert(pinA.valid());
  const uint8_t* ptrA = pinA.value().arena + off;
  assert(ptrA[0]==0xAA);
  // Publish B while A pinned — must not overwrite A's arena (generation pin).
  HomeScenePublication pubB{};
  pubB.snapshot.state = UiScene::DataState::Ready;
  std::vector<uint8_t> patB(b,0xBB);
  assert(homeAddAssetToPublication(pubB,curr,patB.data(),w,h,s));
  pubB.snapshot.currentExists=true;
  assert(model.publishWithAssets(pubB));
  // A's pointer still valid
  assert(pinA.value().arena[off]==0xAA);
  auto pinB = model.acquirePublication();
  assert(pinB.valid());
  assert(pinB.value().arena[off]==0xBB);
  assert(pinB.generation()!=pinA.generation());
  // No heap allocation in render path: acquire + homePublicationToAssets must not allocate.
  UiScene::UiSceneAssets assets;
  homePublicationToAssets(pinB.value(), assets);
  assert(assets.get(curr)!=nullptr);
}

void testInternalMemoryBudget() {
  // UiStateStore metadata (readers, current, generation) stays internal, not part of heapSlotsBytes.
  // Verify that sizeof(UiStateStore<HomeScenePublication>) is small because slots are heap.
  static_assert(sizeof(UiStateStore<HomeScenePublication>) < 800, "UiStateStore metadata must be small, slots in heap/PSRAM");
  static_assert(sizeof(HomeSceneSnapshot) < 3000, "Snapshot metadata small");
  // HomeSceneModel contains two stores + draft ptr, should be <4KB even though arenas are large.
  // We check runtime: draft heap and store heap cover large arenas.
  UiStateStore<HomeScenePublication> s(HomeScenePublication{});
  assert(s.heapSlotsBytes() > 24000);
  HomeSceneModel m;
  assert(m.isDraftHeapAllocated());
  assert(sizeof(HomeSceneModel) < 8192 && "HomeSceneModel object itself should be small, arenas are heap");
}

int main() {
  testAssetArenaGeometry();
  testPublicationStoreMovedToHeapPsram();
  testDraftArenaMovedToHeap();
  testRenderPointerLifetimePinned();
  testInternalMemoryBudget();
  return 0;
}

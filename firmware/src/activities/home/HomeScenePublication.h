#pragma once
// Compatibility alias — canonical definitions now live in ui/pages/HomeSceneModel.h
// This header re-exports the paired publication types for code that already includes it.
#include "ui/pages/HomeSceneModel.h"

namespace HomeScene {
using HomeScenePublicationAlias = HomeScenePublication;
inline bool publicationSlotForKey(const UiScene::AssetKey& key, size_t* o, uint16_t* w, uint16_t* h, uint16_t* s, size_t* b) {
  return homePublicationSlotForKey(key, o, w, h, s, b);
}
inline bool addAssetToPublication(HomeScenePublication& p, const UiScene::AssetKey& k, const uint8_t* d, uint16_t w, uint16_t h, uint16_t s) {
  return homeAddAssetToPublication(p, k, d, w, h, s);
}
inline void publicationToAssets(const HomeScenePublication& p, UiScene::UiSceneAssets& o) {
  return homePublicationToAssets(p, o);
}
}  // namespace HomeScene

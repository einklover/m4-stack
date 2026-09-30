#pragma once
// Procedural 1-bit placeholder covers. Only published for missing real books;
// never written to RecentBooksStore or passed to the book reader.
#include <cstdint>
#include <cstring>
#include "ui/pages/HomeSceneModel.h"

namespace M4HomeFirstBoot {
inline void fillCover(uint8_t* out, uint16_t w, uint16_t h, uint16_t stride, uint8_t motif) {
  std::memset(out, 0, static_cast<size_t>(stride) * h);
  auto dot = [&](int x, int y) {
    if (x >= 0 && y >= 0 && x < w && y < h)
      out[static_cast<size_t>(y) * stride + (x >> 3)] |= static_cast<uint8_t>(0x80u >> (x & 7));
  };
  for (int x=2;x<w-2;++x) { dot(x,2); dot(x,7); dot(x,h-3); }
  for (int y=2;y<h-2;++y) { dot(2,y); dot(w-3,y); }
  const int cx=w/2,cy=h/2;
  for (int d=-w/4;d<=w/4;++d) {
    if (motif%4==0) { dot(cx+d,cy+d);dot(cx+d,cy-d); }
    if (motif%4==1) { dot(cx+d,cy-w/4);dot(cx+d,cy+w/4); }
    if (motif%4==2) dot(cx+d,cy+d*d/(w/6+1)-w/6);
    if (motif%4==3) { dot(cx+d,cy);dot(cx,cy+d); }
  }
  for (int x=w/4;x<3*w/4;++x) dot(x,h-20);
}

inline void publishMissingCovers(HomeScene::HomeScenePublication& pub, size_t realCount) {
  auto publish = [&](const UiScene::AssetKey& key, uint8_t motif) {
    size_t offset=0,bytes=0;
    uint16_t w=0,h=0,stride=0;
    if (!HomeScene::homePublicationSlotForKey(key,&offset,&w,&h,&stride,&bytes)) return;
    uint8_t scratch[HomeScene::kHomeCurrentCoverBytes]{};
    fillCover(scratch,w,h,stride,motif);
    (void)HomeScene::homeAddAssetToPublication(pub,key,scratch,w,h,stride);
  };
  if (realCount==0)
    publish({HomeScene::kBindingCurrentCover,UiScene::kInvalidBindingId,
             UiScene::kInvalidAssetItemIndex},0);
  for (size_t i=(realCount ? realCount-1 : 0);i<3;++i)
    publish({HomeScene::kBindingItemCover,HomeScene::kBindingRecent,
             static_cast<uint8_t>(i)},static_cast<uint8_t>(i+1));
}
} // namespace M4HomeFirstBoot

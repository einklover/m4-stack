// Host regression for the SdFat shared-cache window.
// Links vendored FsCache.cpp (SdFat 2.3.1). The schedule is the FatFile
// partial-sector path: dataCachePrepare returns a pointer, then the caller
// memcpy's. A second task runs in that gap. This is not a FAT mount and not
// a FreeRTOS preemption test.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "common/FsCache.h"

namespace {

constexpr Sector_t kSectorS = 3;
constexpr Sector_t kSectorT = 9;
constexpr uint8_t kPatS = 0xA1;
constexpr uint8_t kPatT = 0x5C;
constexpr uint8_t kWritePayload = 0x3D;

class MemBlockDevice : public FsBlockDeviceInterface {
 public:
  bool isBusy() override { return false; }
  Sector_t sectorCount() override { return kCount; }
  bool syncDevice() override { return true; }

  bool readSector(Sector_t sector, uint8_t* dst) override {
    if (sector >= kCount || dst == nullptr) return false;
    // DMA-style: the device lock covers only this copy, then it is released.
    takeDma();
    std::memcpy(dst, sectors_[sector], 512);
    dmaCopies_++;
    giveDma();
    return true;
  }

  bool writeSector(Sector_t sector, const uint8_t* src) override {
    if (sector >= kCount || src == nullptr) return false;
    takeDma();
    std::memcpy(sectors_[sector], src, 512);
    dmaCopies_++;
    giveDma();
    return true;
  }

  bool readSectors(Sector_t sector, uint8_t* dst, size_t ns) override {
    for (size_t i = 0; i < ns; ++i) {
      if (!readSector(sector + i, dst + i * 512)) return false;
    }
    return true;
  }

  bool writeSectors(Sector_t sector, const uint8_t* src, size_t ns) override {
    for (size_t i = 0; i < ns; ++i) {
      if (!writeSector(sector + i, src + i * 512)) return false;
    }
    return true;
  }

  void fill(Sector_t sector, uint8_t value) { std::memset(sectors_[sector], value, 512); }

  bool sectorIs(Sector_t sector, uint8_t value) const {
    for (int i = 0; i < 512; ++i) {
      if (sectors_[sector][i] != value) return false;
    }
    return true;
  }

  int dmaCopies() const { return dmaCopies_; }
  int dmaDepth() const { return dmaDepth_; }
  int dmaMaxDepth() const { return dmaMaxDepth_; }

 private:
  static constexpr Sector_t kCount = 16;
  uint8_t sectors_[kCount][512]{};
  int dmaHeld_ = 0;
  int dmaDepth_ = 0;
  int dmaMaxDepth_ = 0;
  int dmaCopies_ = 0;

  void takeDma() {
    ++dmaHeld_;
    ++dmaDepth_;
    if (dmaDepth_ > dmaMaxDepth_) dmaMaxDepth_ = dmaDepth_;
  }
  void giveDma() {
    assert(dmaHeld_ > 0);
    --dmaHeld_;
    --dmaDepth_;
  }
};

struct VolumeLock {
  int* held;
  explicit VolumeLock(int* h) : held(h) {
    assert(*held == 0);
    ++(*held);
  }
  ~VolumeLock() { --(*held); }
};

// FatFile.cpp partial read: prepare, then memcpy from the returned pointer.
bool fatStyleRead(FsCache& cache, Sector_t sector, uint8_t* dst, int* volumeHeld, bool holdVolume,
                  void (*interleave)(void*), void* ctx) {
  int local = 0;
  int* held = holdVolume ? volumeHeld : &local;
  VolumeLock lock(held);
  uint8_t* pc = cache.prepare(sector, FsCache::CACHE_FOR_READ);
  if (!pc) return false;
  if (interleave) interleave(ctx);
  std::memcpy(dst, pc, 512);
  return true;
}

bool fatStyleWrite(FsCache& cache, Sector_t sector, uint8_t value, int* volumeHeld, bool holdVolume,
                   void (*interleave)(void*), void* ctx) {
  int local = 0;
  int* held = holdVolume ? volumeHeld : &local;
  VolumeLock lock(held);
  uint8_t* pc = cache.prepare(sector, FsCache::CACHE_FOR_WRITE);
  if (!pc) return false;
  if (interleave) interleave(ctx);
  std::memset(pc, value, 512);
  return cache.sync();
}

struct Interleave {
  FsCache* cache;
  int* volumeHeld;
  bool ran = false;
};

void otherPrepareT(void* raw, uint8_t option, bool checkPattern) {
  auto* ctx = static_cast<Interleave*>(raw);
  if (*ctx->volumeHeld != 0) {
    ctx->ran = false;
    return;
  }
  uint8_t* pc = ctx->cache->prepare(kSectorT, option);
  assert(pc != nullptr);
  if (checkPattern) assert(pc[0] == kPatT);
  ctx->ran = true;
}

void otherReadT(void* raw) { otherPrepareT(raw, FsCache::CACHE_FOR_READ, true); }

// A second writer: prepare keeps CACHE_STATUS_DIRTY on sector T, so T1's
// later memcpy lands in T and sync writes it there.
void otherWriteT(void* raw) { otherPrepareT(raw, FsCache::CACHE_FOR_WRITE, true); }

void reset(MemBlockDevice& dev, FsCache& cache) {
  dev.fill(kSectorS, kPatS);
  dev.fill(kSectorT, kPatT);
  cache.init(&dev);
}

}  // namespace

int main() {
  MemBlockDevice dev;
  FsCache cache;
  int volumeHeld = 0;

  // 1. No volume lock. DMA copies are serialized, but the cache pointer is
  // used after readSector has released the device lock. T2 replaces the
  // single buffer; T1's memcpy observes sector T.
  reset(dev, cache);
  Interleave open{&cache, &volumeHeld, false};
  uint8_t got[512];
  assert(fatStyleRead(cache, kSectorS, got, &volumeHeld, false, otherReadT, &open));
  assert(open.ran);
  assert(got[0] == kPatT);
  assert(cache.sector() == kSectorT);
  assert(dev.dmaMaxDepth() == 1);
  assert(dev.dmaCopies() >= 2);

  // 2. Same schedule with the volume lock covering prepare through memcpy.
  // T2 refuses to enter. T1 reads sector S.
  reset(dev, cache);
  Interleave closed{&cache, &volumeHeld, true};
  assert(fatStyleRead(cache, kSectorS, got, &volumeHeld, true, otherReadT, &closed));
  assert(!closed.ran);
  assert(got[0] == kPatS);
  assert(cache.sector() == kSectorS);

  // 3. Write window. T1 prepares S (dirty). T2 loads T into the same buffer.
  // T1 then stores its payload through the stale pointer, so sync writes that
  // payload to T. Sector S is unchanged.
  reset(dev, cache);
  Interleave writeGap{&cache, &volumeHeld, false};
  assert(fatStyleWrite(cache, kSectorS, kWritePayload, &volumeHeld, false, otherWriteT, &writeGap));
  assert(writeGap.ran);
  assert(dev.sectorIs(kSectorT, kWritePayload));
  assert(dev.sectorIs(kSectorS, kPatS));

  reset(dev, cache);
  Interleave writeHeld{&cache, &volumeHeld, true};
  assert(fatStyleWrite(cache, kSectorS, kWritePayload, &volumeHeld, true, otherWriteT, &writeHeld));
  assert(!writeHeld.ran);
  assert(dev.sectorIs(kSectorS, kWritePayload));
  assert(dev.sectorIs(kSectorT, kPatT));

  std::cout << "sd_cache_interleave ok dma_copies=" << dev.dmaCopies() << "\n";
  return 0;
}

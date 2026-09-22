#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

// Murphy M4 PSRAM architecture.
//
// Internal RAM is protected system memory. Large application state lives in
// three PSRAM arenas allocated once during setup:
//   TTF     2.5 MiB  font metadata/glyph/cache state; never reset at Home
//   App     2.5 MiB  reader/plugin/network/json working state
//   Scratch 1.0 MiB  short-lived image/codec/conversion work
//
// App + Scratch are reset only after the old Activity and provider workers are
// gone. Large application allocations never fall back to internal RAM.

#ifndef M4_PSRAM_TTF_ARENA_BYTES
#define M4_PSRAM_TTF_ARENA_BYTES (2560u * 1024u)
#endif
#ifndef M4_PSRAM_APP_ARENA_BYTES
#define M4_PSRAM_APP_ARENA_BYTES (2560u * 1024u)
#endif
#ifndef M4_PSRAM_SCRATCH_ARENA_BYTES
#define M4_PSRAM_SCRATCH_ARENA_BYTES (1024u * 1024u)
#endif

namespace M4Memory {

enum class Pool : uint8_t { Ttf = 0, App = 1, Scratch = 2 };

struct PoolStats {
  size_t capacity = 0;
  size_t used = 0;
  size_t peak = 0;
  size_t largestFree = 0;
  uint32_t allocations = 0;
  uint32_t failures = 0;
  uint32_t resets = 0;
  bool ready = false;
};

namespace detail {

constexpr size_t kAlign = 16;
constexpr uint32_t kBlockMagic = 0x4D345052u;  // M4PR

inline size_t alignUp(size_t n) {
  return (n + (kAlign - 1)) & ~(kAlign - 1);
}

inline void* directPsramAlloc(size_t n) {
  if (!n) return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return std::malloc(n);
#endif
}

inline void* directPsramRealloc(void* p, size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  if (!p) return directPsramAlloc(n);
  if (!n) {
    heap_caps_free(p);
    return nullptr;
  }
  return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return std::realloc(p, n);
#endif
}

inline void directFree(void* p) {
  if (!p) return;
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}

class Arena {
 public:
  bool begin(size_t bytes) {
    if (ready_) return true;
    if (bytes < sizeof(Block) + 64) return false;

    raw_ = static_cast<uint8_t*>(directPsramAlloc(bytes + kAlign));
    if (!raw_) {
      ++failures_;
      return false;
    }

    const uintptr_t aligned =
        (reinterpret_cast<uintptr_t>(raw_) + (kAlign - 1)) &
        ~(uintptr_t(kAlign - 1));
    base_ = reinterpret_cast<uint8_t*>(aligned);
    capacity_ = bytes;

#if defined(ARDUINO_ARCH_ESP32)
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
      directFree(raw_);
      raw_ = nullptr;
      base_ = nullptr;
      capacity_ = 0;
      ++failures_;
      return false;
    }
#endif

    resetUnlocked(false);
    ready_ = true;
    return true;
  }

  bool ready() const { return ready_; }

  bool owns(const void* p) const {
    if (!ready_ || !p) return false;
    const auto* b = static_cast<const uint8_t*>(p);
    return b >= base_ + sizeof(Block) && b < base_ + capacity_;
  }

  void* alloc(size_t requested) {
    if (!requested || !ready_) return nullptr;
    Lock guard(*this);
    return allocUnlocked(requested);
  }

  void* reallocPtr(void* ptr, size_t requested) {
    if (!ptr) return alloc(requested);
    if (!requested) {
      freePtr(ptr);
      return nullptr;
    }
    if (!owns(ptr)) return nullptr;

    Lock guard(*this);
    Block* b = blockFor(ptr);
    if (!validAllocated(b)) return nullptr;

    const size_t oldRequested = b->requested;
    const size_t need = alignUp(requested);

    if (need <= b->size) {
      used_ -= oldRequested;
      used_ += requested;
      b->requested = requested;
      splitBlock(b, need);
      return ptr;
    }

    if (b->next && b->next->free &&
        b->size + sizeof(Block) + b->next->size >= need) {
      mergeNext(b);
      used_ -= oldRequested;
      used_ += requested;
      b->requested = requested;
      splitBlock(b, need);
      if (used_ > peak_) peak_ = used_;
      return ptr;
    }

    void* replacement = allocUnlocked(requested);
    if (!replacement) return nullptr;
    std::memcpy(replacement, ptr,
                oldRequested < requested ? oldRequested : requested);
    freeUnlocked(ptr);
    return replacement;
  }

  void freePtr(void* ptr) {
    if (!ptr || !owns(ptr)) return;
    Lock guard(*this);
    freeUnlocked(ptr);
  }

  void reset() {
    if (!ready_) return;
    Lock guard(*this);
    resetUnlocked(true);
  }

  PoolStats stats() {
    PoolStats out;
    if (!ready_) {
      out.failures = failures_;
      return out;
    }
    Lock guard(*this);
    out.capacity = capacity_;
    out.used = used_;
    out.peak = peak_;
    out.allocations = allocations_;
    out.failures = failures_;
    out.resets = resets_;
    out.ready = true;
    size_t largest = 0;
    for (Block* b = head_; b; b = b->next) {
      if (b->free && b->size > largest) largest = b->size;
    }
    out.largestFree = largest;
    return out;
  }

 private:
  struct alignas(16) Block {
    size_t size = 0;
    size_t requested = 0;
    Block* prev = nullptr;
    Block* next = nullptr;
    uint32_t magic = kBlockMagic;
    bool free = true;
  };

  class Lock {
   public:
    explicit Lock(Arena& arena) : arena_(arena) {
#if defined(ARDUINO_ARCH_ESP32)
      if (arena_.mutex_) xSemaphoreTake(arena_.mutex_, portMAX_DELAY);
#endif
    }
    ~Lock() {
#if defined(ARDUINO_ARCH_ESP32)
      if (arena_.mutex_) xSemaphoreGive(arena_.mutex_);
#endif
    }

   private:
    Arena& arena_;
  };

  bool validAllocated(Block* b) const {
    return b && b->magic == kBlockMagic && !b->free;
  }

  Block* blockFor(void* p) const {
    return reinterpret_cast<Block*>(static_cast<uint8_t*>(p) - sizeof(Block));
  }

  void resetUnlocked(bool countReset) {
    head_ = reinterpret_cast<Block*>(base_);
    head_->size = capacity_ - sizeof(Block);
    head_->requested = 0;
    head_->prev = nullptr;
    head_->next = nullptr;
    head_->magic = kBlockMagic;
    head_->free = true;
    used_ = 0;
    if (countReset) ++resets_;
  }

  void splitBlock(Block* b, size_t payloadBytes) {
    if (!b || b->size <= payloadBytes) return;
    const size_t remain = b->size - payloadBytes;
    if (remain < sizeof(Block) + 32) return;

    auto* next = reinterpret_cast<Block*>(
        reinterpret_cast<uint8_t*>(b) + sizeof(Block) + payloadBytes);
    next->size = remain - sizeof(Block);
    next->requested = 0;
    next->prev = b;
    next->next = b->next;
    next->magic = kBlockMagic;
    next->free = true;
    if (next->next) next->next->prev = next;

    b->next = next;
    b->size = payloadBytes;
  }

  void mergeNext(Block* b) {
    if (!b || !b->next || !b->next->free) return;
    Block* n = b->next;
    b->size += sizeof(Block) + n->size;
    b->next = n->next;
    if (b->next) b->next->prev = b;
  }

  void* allocUnlocked(size_t requested) {
    const size_t need = alignUp(requested);
    for (Block* b = head_; b; b = b->next) {
      if (!b->free || b->size < need) continue;
      splitBlock(b, need);
      b->free = false;
      b->requested = requested;
      used_ += requested;
      if (used_ > peak_) peak_ = used_;
      ++allocations_;
      return reinterpret_cast<uint8_t*>(b) + sizeof(Block);
    }
    ++failures_;
    return nullptr;
  }

  void freeUnlocked(void* ptr) {
    Block* b = blockFor(ptr);
    if (!validAllocated(b)) return;

    if (used_ >= b->requested) used_ -= b->requested;
    else used_ = 0;

    b->requested = 0;
    b->free = true;

    if (b->next && b->next->free) mergeNext(b);
    if (b->prev && b->prev->free) {
      b = b->prev;
      mergeNext(b);
    }
  }

  uint8_t* raw_ = nullptr;
  uint8_t* base_ = nullptr;
  size_t capacity_ = 0;
  Block* head_ = nullptr;
  size_t used_ = 0;
  size_t peak_ = 0;
  uint32_t allocations_ = 0;
  uint32_t failures_ = 0;
  uint32_t resets_ = 0;
  bool ready_ = false;
#if defined(ARDUINO_ARCH_ESP32)
  SemaphoreHandle_t mutex_ = nullptr;
#endif
};

class Manager {
 public:
  bool begin() {
    if (started_) return fullyReady();
    started_ = true;

    const bool ttfOk = ttf_.begin(M4_PSRAM_TTF_ARENA_BYTES);
    const bool appOk = app_.begin(M4_PSRAM_APP_ARENA_BYTES);
    const bool scratchOk = scratch_.begin(M4_PSRAM_SCRATCH_ARENA_BYTES);
    return ttfOk && appOk && scratchOk;
  }

  bool started() const { return started_; }
  bool fullyReady() const {
    return ttf_.ready() && app_.ready() && scratch_.ready();
  }

  Arena& arena(Pool pool) {
    if (pool == Pool::Ttf) return ttf_;
    if (pool == Pool::Scratch) return scratch_;
    return app_;
  }

  bool ownsAny(const void* p) const {
    return ttf_.owns(p) || app_.owns(p) || scratch_.owns(p);
  }

 private:
  bool started_ = false;
  Arena ttf_;
  Arena app_;
  Arena scratch_;
};

inline Manager& manager() {
  static Manager instance;
  return instance;
}

inline void* allocManaged(Pool pool, size_t n) {
  if (!n) return nullptr;
  Manager& m = manager();
  Arena& a = m.arena(pool);
  if (a.ready()) return a.alloc(n);

  // Degraded boot / host test. This is still external-only on ESP32.
  return directPsramAlloc(n);
}

inline void* reallocManaged(Pool pool, void* ptr, size_t n) {
  if (!ptr) return allocManaged(pool, n);

  Manager& m = manager();
  Arena& a = m.arena(pool);
  if (a.ready() && a.owns(ptr)) return a.reallocPtr(ptr, n);

  // A direct allocation made before begin() may still be resized later.
  // Never reinterpret a pointer owned by another arena.
  if (!m.ownsAny(ptr)) return directPsramRealloc(ptr, n);
  return nullptr;
}

inline void freeManaged(void* ptr) {
  if (!ptr) return;
  Manager& m = manager();

  if (m.arena(Pool::Ttf).owns(ptr)) {
    m.arena(Pool::Ttf).freePtr(ptr);
  } else if (m.arena(Pool::App).owns(ptr)) {
    m.arena(Pool::App).freePtr(ptr);
  } else if (m.arena(Pool::Scratch).owns(ptr)) {
    m.arena(Pool::Scratch).freePtr(ptr);
  } else {
    directFree(ptr);
  }
}

}  // namespace detail

inline bool begin() { return detail::manager().begin(); }
inline bool ready() { return detail::manager().fullyReady(); }

inline void* allocTtf(size_t n) {
  return detail::allocManaged(Pool::Ttf, n);
}
inline void* reallocTtf(void* p, size_t n) {
  return detail::reallocManaged(Pool::Ttf, p, n);
}

inline void* allocApp(size_t n) {
  return detail::allocManaged(Pool::App, n);
}
inline void* reallocApp(void* p, size_t n) {
  return detail::reallocManaged(Pool::App, p, n);
}

inline void* allocScratch(size_t n) {
  return detail::allocManaged(Pool::Scratch, n);
}
inline void* reallocScratch(void* p, size_t n) {
  return detail::reallocManaged(Pool::Scratch, p, n);
}

inline void free(void* p) { detail::freeManaged(p); }

inline void resetApp() {
  detail::manager().arena(Pool::App).reset();
}
inline void resetScratch() {
  detail::manager().arena(Pool::Scratch).reset();
}
inline void resetTransient() {
  resetApp();
  resetScratch();
}

inline PoolStats stats(Pool pool) {
  return detail::manager().arena(pool).stats();
}

inline size_t reservedBytes() {
  const PoolStats t = stats(Pool::Ttf);
  const PoolStats a = stats(Pool::App);
  const PoolStats s = stats(Pool::Scratch);
  return t.capacity + a.capacity + s.capacity;
}

inline size_t usedBytes() {
  const PoolStats t = stats(Pool::Ttf);
  const PoolStats a = stats(Pool::App);
  const PoolStats s = stats(Pool::Scratch);
  return t.used + a.used + s.used;
}

}  // namespace M4Memory

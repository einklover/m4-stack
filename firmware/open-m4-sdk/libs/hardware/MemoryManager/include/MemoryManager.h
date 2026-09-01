#pragma once

// FreeInk SDK — on-demand memory / cache reclaim.
//
// A small, app-neutral registry of "cache sinks" plus heap reporting, so a
// consumer can free RAM on demand (a control-center "clear caches" action) or
// under memory pressure. Components that hold rebuildable RAM caches — rendered
// pages, decoded images, glyph atlases, parsed-document buffers, PSRAM pools —
// register a sink with an eviction callback; clearCaches() then asks each sink
// (lowest priority first) to release memory until a target is met.
//
// The design mirrors the memory-manager pattern common to e-reader firmware:
// a priority-ordered set of evictable caches driven by free-heap measurement,
// soft/hard used-bytes watermarks on the scarce internal pool, named static
// task-stack slots that big-stack transient tasks (radio bring-up, sync)
// borrow and return without fragmenting the heap, small fixed bump arenas for
// phase-scoped scratch, and a reboot-flavored "boost" for when only a clean
// software restart can undo fragmentation. Nothing here is board-specific; it
// is a thin wrapper over the ESP-IDF heap-capabilities allocator and FreeRTOS
// static-task primitives.

#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace freeink {

enum class MemPool : uint8_t { Internal, Psram, Default };

enum class MemPressure : uint8_t { None, Soft, Hard };

struct TaskStack {
  void* stack = nullptr;
  void* tcb = nullptr;
  size_t stackBytes = 0;
};

struct CacheSink {
  const char* name = nullptr;
  uint8_t priority = 128;
  std::function<size_t(size_t bytesRequested)> evict;
};

class MemoryManager {
 public:
  static constexpr int kMaxSinks = 12;
  static constexpr int kMaxStackSlots = 6;
  static constexpr int kMaxArenas = 4;

  static MemoryManager& instance();

  int registerSink(const CacheSink& sink);
  void unregisterSink(int id);
  void unregisterSink(const char* name);

  size_t freeBytes(MemPool pool = MemPool::Default) const;
  size_t largestFreeBlock(MemPool pool = MemPool::Default) const;
  size_t minEverFree(MemPool pool = MemPool::Default) const;

  size_t clearCaches(size_t bytesTarget = 0);
  size_t boost(size_t* freeBefore = nullptr, size_t* freeAfter = nullptr, MemPool pool = MemPool::Default);
  [[noreturn]] void rebootBoost();

  void setWatermarks(uint8_t softPct = 60, uint8_t hardPct = 75);
  MemPressure pressure() const;
  size_t relievePressure();
  bool ensureFree(size_t bytes, MemPool pool = MemPool::Default);

  TaskStack acquireTaskStack(const char* slot, const char* owner, size_t stackBytes);
  void releaseTaskStack(const char* slot);

  int arenaCreate(size_t bytes, MemPool pool = MemPool::Internal);
  void* arenaAlloc(int id, size_t bytes, size_t align = 4);
  void arenaReset(int id);
  void arenaDestroy(int id);
  size_t arenaRemaining(int id) const;

 private:
  MemoryManager() = default;

  struct Entry {
    CacheSink sink;
    int id = 0;
    bool used = false;
  };
  Entry _sinks[kMaxSinks];
  int _nextId = 1;

  struct StackSlot {
    const char* name = nullptr;
    void* stack = nullptr;
    void* tcb = nullptr;
    size_t stackBytes = 0;
    bool owned = false;
  };
  StackSlot _stacks[kMaxStackSlots];

  struct Arena {
    uint8_t* base = nullptr;
    size_t size = 0;
    size_t used = 0;
    bool live = false;
  };
  Arena _arenas[kMaxArenas];

  size_t _internalTotal = 0;
  size_t _softUsed = 0;
  size_t _hardUsed = 0;
};

}  // namespace freeink

#pragma once

// Lua 5.4 memory + instruction/time budget helpers for M4x host.
// Host-testable: only depends on Lua C API (and a clock callback).

#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
struct lua_State;
struct lua_Debug;
}

namespace M4xLuaSandbox {

// Heap budget for one app session (Lua allocations only).
inline constexpr size_t kDefaultHeapLimit = 512 * 1024;
// PSRAM-backed Lua allocations can safely use a bounded larger working set;
// keep the no-PSRAM device limit unchanged so TLS/UI still have headroom.
inline constexpr size_t kPsramHeapLimit = 768 * 1024;
// Instruction steps between hook checks (mask count).
inline constexpr int kHookStep = 10000;
// Max VM instructions per protected callback (approx; counted in steps of kHookStep).
inline constexpr uint32_t kDefaultInstrBudget = 2'000'000;
// Wall-clock soft limit per callback (ms). 0 disables.
inline constexpr uint32_t kDefaultWallMs = 8000;
// A loading tick may execute one bounded network hop (API requests are capped
// at 30 s). Keep the normal UI callback limit strict, but do not abort a
// deliberately loading-state tick at 8 s.
inline constexpr uint32_t kNetworkWallMs = kDefaultWallMs * 4;
// Longer wall for start()/init which may load scripts.
inline constexpr uint32_t kStartWallMs = 30000;

struct Budget {
  size_t memUsed = 0;
  size_t memPeak = 0;
  size_t memLimit = kDefaultHeapLimit;

  uint32_t instrBudget = kDefaultInstrBudget;
  uint32_t instrSpent = 0;

  uint32_t wallLimitMs = kDefaultWallMs;
  uint32_t wallStartMs = 0;
  uint32_t (*nowMs)() = nullptr;

  // Cooperative cancel (set from another task; checked in instruction hook).
  // Points at host-owned atomic; never freed by sandbox.
  std::atomic<bool>* cancelFlag = nullptr;

  bool violated = false;
  const char* reason = nullptr;  // static string key
};

// Lua allocator: tracks usage; returns nullptr when over memLimit (Lua raises error).
void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);

// Instruction/count hook: enforces instrBudget and optional wall clock.
void countHook(lua_State* L, lua_Debug* ar);

// Install hook on L; budget must outlive L.
void installHook(lua_State* L, Budget* budget);

// Begin a protected callback window (reset instrSpent / wallStart / violated).
void beginCallback(Budget* budget, uint32_t wallLimitMs);

// Full read into buffer; returns false on short/failed read.
// Caller provides readFn(ctx, dest, n) -> bytes read (0 = EOF/error).
using ReadFn = int (*)(void* ctx, uint8_t* dest, size_t n);
bool readExact(ReadFn readFn, void* ctx, uint8_t* dest, size_t n);

// Full write; returns false on short/failed write.
using WriteFn = int (*)(void* ctx, const uint8_t* src, size_t n);
bool writeExact(WriteFn writeFn, void* ctx, const uint8_t* src, size_t n);

}  // namespace M4xLuaSandbox

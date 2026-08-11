#include "apps/M4xLuaSandbox.h"

#include <cstdlib>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace M4xLuaSandbox {
namespace {

Budget* budgetFromState(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "m4x_budget");
  Budget* b = static_cast<Budget*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return b;
}

}  // namespace

void* alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  auto* b = static_cast<Budget*>(ud);
  if (!b) {
    if (nsize == 0) {
      free(ptr);
      return nullptr;
    }
    return realloc(ptr, nsize);
  }

  // Lua 5.4: when ptr!=NULL and nsize==0, free; when ptr==NULL, osize is type tag (not size).
  if (nsize == 0) {
    if (ptr) {
      if (b->memUsed >= osize) b->memUsed -= osize;
      else b->memUsed = 0;
#if defined(ARDUINO_ARCH_ESP32)
      // Lua consists mostly of sub-4 KiB allocations. The default Arduino
      // malloc policy keeps those in scarce internal RAM and can starve TLS.
      heap_caps_free(ptr);
#else
      free(ptr);
#endif
    }
    return nullptr;
  }

  size_t newUsed = b->memUsed;
  if (ptr) {
    // realloc: release osize, take nsize
    if (newUsed >= osize) newUsed -= osize;
    else newUsed = 0;
  }
  // else: fresh alloc — osize is not a byte size
  if (newUsed + nsize > b->memLimit) {
    b->violated = true;
    b->reason = "lua_mem_limit";
    return nullptr;
  }

#if defined(ARDUINO_ARCH_ESP32)
  // Keep the Lua VM in PSRAM so mbedTLS retains contiguous internal memory.
  // heap_caps_realloc can also migrate a prior fallback allocation.
  void* p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
#else
  void* p = realloc(ptr, nsize);
#endif
  if (!p) {
    b->violated = true;
    b->reason = "lua_oom";
    return nullptr;
  }
  newUsed += nsize;
  b->memUsed = newUsed;
  if (newUsed > b->memPeak) b->memPeak = newUsed;
  return p;
}

void countHook(lua_State* L, lua_Debug* /*ar*/) {
  Budget* b = budgetFromState(L);
  if (!b || b->violated) return;

  if (b->cancelFlag && b->cancelFlag->load(std::memory_order_relaxed)) {
    b->violated = true;
    b->reason = "lua_cancel";
    luaL_error(L, "cancelled");
    return;
  }

  b->instrSpent += static_cast<uint32_t>(kHookStep);
  if (b->instrSpent >= b->instrBudget) {
    b->violated = true;
    b->reason = "lua_instr_limit";
    luaL_error(L, "instruction budget exceeded");
    return;
  }
  if (b->wallLimitMs > 0 && b->nowMs) {
    const uint32_t now = b->nowMs();
    if (now - b->wallStartMs > b->wallLimitMs) {
      b->violated = true;
      b->reason = "lua_time_limit";
      luaL_error(L, "callback time budget exceeded");
    }
  }
}

void installHook(lua_State* L, Budget* budget) {
  if (!L || !budget) return;
  lua_pushlightuserdata(L, budget);
  lua_setfield(L, LUA_REGISTRYINDEX, "m4x_budget");
  lua_sethook(L, countHook, LUA_MASKCOUNT, kHookStep);
}

void beginCallback(Budget* budget, uint32_t wallLimitMs) {
  if (!budget) return;
  budget->instrSpent = 0;
  budget->wallLimitMs = wallLimitMs;
  budget->violated = false;
  budget->reason = nullptr;
  if (budget->nowMs) budget->wallStartMs = budget->nowMs();
  else budget->wallStartMs = 0;
}

bool readExact(ReadFn readFn, void* ctx, uint8_t* dest, size_t n) {
  if (!readFn || (!dest && n > 0)) return false;
  size_t off = 0;
  while (off < n) {
    const int r = readFn(ctx, dest + off, n - off);
    if (r <= 0) return false;
    off += static_cast<size_t>(r);
  }
  return true;
}

bool writeExact(WriteFn writeFn, void* ctx, const uint8_t* src, size_t n) {
  if (!writeFn || (!src && n > 0)) return false;
  size_t off = 0;
  while (off < n) {
    const int w = writeFn(ctx, src + off, n - off);
    if (w <= 0) return false;
    off += static_cast<size_t>(w);
  }
  return true;
}

}  // namespace M4xLuaSandbox

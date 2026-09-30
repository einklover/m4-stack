#include "apps/providers/M4Psram.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
size_t gInternalFree = 256u * 1024u;
size_t gInternalLargest = 256u * 1024u;
unsigned gInternalAllocs = 0;
unsigned gInternalTaskCreates = 0;
}

void* heap_caps_malloc(size_t bytes, uint32_t caps) {
  if (caps & MALLOC_CAP_SPIRAM) return nullptr;
  ++gInternalAllocs;
  return std::malloc(bytes);
}

void* heap_caps_realloc(void*, size_t, uint32_t) { return nullptr; }
void heap_caps_free(void* ptr) { std::free(ptr); }
size_t heap_caps_get_free_size(uint32_t) { return gInternalFree; }
size_t heap_caps_get_largest_free_block(uint32_t) { return gInternalLargest; }
BaseType_t xTaskCreateWithCaps(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t,
                               TaskHandle_t*, uint32_t caps) {
  if (caps & MALLOC_CAP_INTERNAL) ++gInternalTaskCreates;
  return pdFAIL;
}
void vTaskDeleteWithCaps(TaskHandle_t) {}

int main() {
  // A failed PSRAM resize must copy the old payload before releasing it.
  auto* old = static_cast<uint8_t*>(heap_caps_malloc(32, MALLOC_CAP_INTERNAL));
  assert(old);
  for (uint8_t i = 0; i < 32; ++i) old[i] = static_cast<uint8_t>(0x40u + i);
  auto* grown = static_cast<uint8_t*>(M4Psram::reallocPrefer(old, 32, 64, "test-realloc"));
  assert(grown && grown != old);
  for (uint8_t i = 0; i < 32; ++i) assert(grown[i] == static_cast<uint8_t>(0x40u + i));
  M4Psram::freePrefer(grown);

  // Large PSRAM-first requests must not consume the TLS/internal reserve.
  gInternalAllocs = 0;
  const auto beforeLarge = M4Psram::allocationStats();
  assert(M4Psram::mallocPrefer(32u * 1024u, "test-large") == nullptr);
  assert(gInternalAllocs == 0);
  const auto afterLarge = M4Psram::allocationStats();
  assert(afterLarge.largeFallbackBlocked == beforeLarge.largeFallbackBlocked + 1);

  TaskHandle_t task = nullptr;
  const auto beforeLargeStack = M4Psram::allocationStats();
  assert(M4Psram::createTask(+[](void*) {}, "test-large-stack", 32u * 1024u,
                             nullptr, 1, &task) == pdFAIL);
  assert(gInternalTaskCreates == 0);
  const auto afterLargeStack = M4Psram::allocationStats();
  assert(afterLargeStack.taskFailures == beforeLargeStack.taskFailures + 1);
  assert(afterLargeStack.largeFallbackBlocked == beforeLargeStack.largeFallbackBlocked + 1);

  // Small compatibility fallbacks are still allowed when they leave the floor.
  gInternalFree = 80u * 1024u;
  gInternalLargest = 80u * 1024u;
  void* small = M4Psram::mallocPrefer(8u * 1024u, "test-small");
  assert(small);
  M4Psram::freePrefer(small);

  gInternalFree = 70u * 1024u;
  gInternalLargest = 70u * 1024u;
  gInternalAllocs = 0;
  const auto beforeFloor = M4Psram::allocationStats();
  assert(M4Psram::mallocPrefer(8u * 1024u, "test-floor") == nullptr);
  assert(gInternalAllocs == 0);
  const auto afterFloor = M4Psram::allocationStats();
  assert(afterFloor.failures == beforeFloor.failures + 1);
  const auto beforeTaskFloor = M4Psram::allocationStats();
  assert(M4Psram::createTask(+[](void*) {}, "test-small-stack-floor", 8u * 1024u,
                             nullptr, 1, &task) == pdFAIL);
  assert(gInternalTaskCreates == 0);
  const auto afterTaskFloor = M4Psram::allocationStats();
  assert(afterTaskFloor.taskFailures == beforeTaskFloor.taskFailures + 1);
  std::cout << "M4Psram policy PASS\n";
}

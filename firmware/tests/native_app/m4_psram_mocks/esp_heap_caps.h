#pragma once

#include <cstddef>
#include <cstdint>

using UBaseType_t = unsigned;
using BaseType_t = int;
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);

inline constexpr uint32_t MALLOC_CAP_SPIRAM = 1u << 0;
inline constexpr uint32_t MALLOC_CAP_INTERNAL = 1u << 1;
inline constexpr uint32_t MALLOC_CAP_8BIT = 1u << 2;

void* heap_caps_malloc(size_t bytes, uint32_t caps);
void* heap_caps_realloc(void* ptr, size_t bytes, uint32_t caps);
void heap_caps_free(void* ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);

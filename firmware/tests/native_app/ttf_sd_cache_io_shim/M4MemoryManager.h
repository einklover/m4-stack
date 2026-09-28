#pragma once
#include <cstdlib>
namespace M4Memory {
inline void* allocTtf(size_t n) { return std::malloc(n); }
inline void free(void* p) { std::free(p); }
}

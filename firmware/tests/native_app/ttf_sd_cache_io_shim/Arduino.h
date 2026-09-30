#pragma once
#include <cstdio>
#include <cassert>
using SemaphoreHandle_t = int*;
constexpr int pdTRUE = 1;
inline int pdMS_TO_TICKS(int x) { return x; }
inline int* xSemaphoreCreateMutex() {
  static int mu;
  return &mu;
}
inline int xSemaphoreTake(int* p, int) { assert(!*p); *p=1; return 1; }
inline void xSemaphoreGive(int* p) { assert(*p==1); *p=0; }
inline void vSemaphoreDelete(int*) {}
struct SerialType {
  template <class... T>
  void printf(const char*, T...) {}
};
inline SerialType Serial;

inline unsigned long fakeMillis = 0;
inline unsigned long millis() { return fakeMillis; }

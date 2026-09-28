#pragma once
#include <cstdio>
using SemaphoreHandle_t = int*;
constexpr int pdTRUE = 1;
inline int pdMS_TO_TICKS(int x) { return x; }
inline int* xSemaphoreCreateMutex() {
  static int mu;
  return &mu;
}
inline int xSemaphoreTake(int*, int) { return 1; }
inline void xSemaphoreGive(int*) {}
inline void vSemaphoreDelete(int*) {}
struct SerialType {
  template <class... T>
  void printf(const char*, T...) {}
};
inline SerialType Serial;

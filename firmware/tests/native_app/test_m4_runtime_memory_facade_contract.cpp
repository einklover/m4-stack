#include <type_traits>

#include "../../src/util/M4RuntimeMemory.h"

int main() {
  static_assert(std::is_same_v<decltype(&m4CaptureRuntimeMemory), M4RuntimeMemorySnapshot (*)()>);
  static_assert(std::is_same_v<decltype(&m4LogRuntimeMemory), void (*)(const char*)>);
  return 0;
}

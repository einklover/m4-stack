#include <cassert>

#include "util/M4SubActivityLifecycle.h"

int main() {
  assert(M4SubActivityLifecycle::verifyDeferredDestroy());
  assert(M4SubActivityLifecycle::verifyNestedMenuDeferredClose());
  return 0;
}

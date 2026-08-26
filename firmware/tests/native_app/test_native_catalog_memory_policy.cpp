#include "apps/providers/M4NativeCatalogPolicy.h"
#include <cassert>
#include <cstddef>
#include <iostream>

int main() {
  using namespace M4NativeCatalogPolicy;
  assert(kTaskStackBytes >= 72u * 1024u);
  assert(!preferPsramAssembly("fanqie"));
  assert(preferPsramAssembly("jjwxc"));
  assert(preferPsramAssembly("weread"));
  assert(preferPsramAssembly("legado"));
  std::cout << "native catalog memory policy OK\n";
  return 0;
}

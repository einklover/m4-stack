#include <cassert>
#include "../../src/util/M4CacheClearPolicy.h"

int main() {
  using M4CacheClearPolicy::keepReaderProgress;
  assert(keepReaderProgress("progress.bin", false)); // EPUB/XTC, TXT legacy
  assert(keepReaderProgress("progress.dat", false)); // TXT current
  assert(keepReaderProgress("progress.tmp", false)); // TXT crash recovery
  assert(!keepReaderProgress("progress.dat", true));
  assert(!keepReaderProgress("book.bin", false));
  assert(!keepReaderProgress(nullptr, false));
}

#include "util/M4FontDebugPolicy.h"

#include <cassert>
#include <cstdint>

int main() {
  using M4FontDebugPolicy::isCompleteRuntimeFont;
  using M4FontDebugPolicy::isRuntimeFilename;

  assert(!isCompleteRuntimeFont(0, "empty", "empty"));
  assert(!isCompleteRuntimeFont(27017216, "sfnt", "table_past_eof"));
  assert(isCompleteRuntimeFont(4268876, "sfnt", "tables_in_file"));
  assert(isRuntimeFilename("正常 字体.ttf"));
  assert(!isRuntimeFilename(".partial.ttf"));
  return 0;
}

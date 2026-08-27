#include "util/M4FontDebugPolicy.h"

#include <cstdlib>
#include <iostream>

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::cerr << "m4_font_debug_policy FAIL: " #cond "\n"; \
      return 1; \
    } \
  } while (0)

int main() {
  using namespace M4FontDebugPolicy;
  CHECK(isRuntimeFilename("Readable.ttf"));
  CHECK(isRuntimeFilename("集合.OTC"));
  CHECK(!isRuntimeFilename(".hidden.ttf"));
  CHECK(!isRuntimeFilename("cover.png"));

  CHECK(validateBasename("Readable.ttf").empty());
  CHECK(validateBasename("中文 字体.otf").empty());
  CHECK(!validateBasename("../Readable.ttf").empty());
  CHECK(!validateBasename("/FONT/Readable.ttf").empty());
  CHECK(!validateBasename(".hidden.ttf").empty());

  const Candidate candidates[] = {{"Readable.ttf", "Readable.ttf", "ttf"},
                                  {"中文 字体.otf", "中文 字体.otf", "otf"}};
  CHECK(findCandidate(candidates, 2, "中文 字体.otf") == &candidates[1]);
  CHECK(findCandidate(candidates, 2, "missing.ttf") == nullptr);
  std::cout << "m4 font debug policy tests passed\n";
  return 0;
}

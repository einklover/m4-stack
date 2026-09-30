#undef NDEBUG
#include "apps/M4xPathSafe.h"
#include <cassert>
#include <string>
#include <vector>
int main() {
  std::vector<std::string> files = {"main.lua", "icon_home.bmp"};
  for (int i=0; i<156; ++i) files.push_back("art/card_"+std::to_string(i)+".bmp");
  auto r = M4xPathSafe::makeExtractList("main.lua", "icon_home.bmp", files);
  assert(r.ok && r.paths.size()==159);
  for (int i=156; i<164; ++i) files.push_back("art/card_"+std::to_string(i)+".bmp");
  r = M4xPathSafe::makeExtractList("main.lua", "icon_home.bmp", files);
  assert(!r.ok && r.error=="too_many_files");
}

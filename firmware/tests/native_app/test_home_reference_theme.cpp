#include <assert.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static char* readFile(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  assert(f);
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::rewind(f);
  char* s = static_cast<char*>(std::malloc(static_cast<size_t>(n) + 1));
  assert(s);
  assert(std::fread(s, 1, static_cast<size_t>(n), f) == static_cast<size_t>(n));
  s[n] = '\0';
  std::fclose(f);
  return s;
}

static void require(const char* s, const char* needle) {
  assert(std::strstr(s, needle) != nullptr && "reference Home theme contract missing");
}

int main(int argc, char** argv) {
  assert(argc == 3);
  char* cpp = readFile(argv[1]);
  char* hdr = readFile(argv[2]);

  require(cpp, "makeFengyanRecentLayout");
  require(cpp, "layout.heroCover");
  require(cpp, "layout.miniCover");
  require(cpp, "\"最近阅读\"");
  assert(std::strstr(cpp, "\"快 捷 操 作 \"") == nullptr);
  require(cpp, "kHomeQuickColumns = 4");
  require(cpp, "kHomeQuickHeaderOffset = 0");
  require(cpp, "kNoReadingHistory");
  require(cpp, "HomeRef::FocusInset");
  require(cpp, "drawLineIconFolder");
  require(cpp, "drawLineIconWeread");
  require(cpp, "drawLineIconTomato");
  require(cpp, "drawLineIconJinjiang");
  require(cpp, "drawWifiGlyph");
  require(cpp, "homeFooter");
  require(cpp, "\"历史\"");
  require(cpp, "HomeRef::BottomY");
  require(cpp, "HomeRef::BottomSplit1");
  require(cpp, "HomeRef::BottomSplit2");
  require(hdr, ".homeRecentBooksCount = 4");
  require(hdr, ".homeCoverWidth = 171");
  require(hdr, ".homeCoverThumbHeight = 254");

  std::free(cpp);
  std::free(hdr);
  std::puts("home reference theme contract passed");
  return 0;
}

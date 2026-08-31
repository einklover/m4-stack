// Host tests for utf8EllipsizeChars: drawer labels show ≤4 codepoints in
// full and append an ellipsis only when the string is longer.
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/lib/Utf8 \
//     firmware/lib/Utf8/Utf8.cpp firmware/tests/native_app/test_utf8_ellipsize.cpp

#include "Utf8.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
  assert(utf8EllipsizeChars("文件管理", 4) == "文件管理");
  assert(utf8EllipsizeChars("阅读历史", 4) == "阅读历史");
  assert(utf8EllipsizeChars("网络管理", 4) == "网络管理");
  assert(utf8EllipsizeChars("系统设置", 4) == "系统设置");
  assert(utf8EllipsizeChars("微信读书", 4) == "微信读书");
  assert(utf8EllipsizeChars("番茄小说", 4) == "番茄小说");
  assert(utf8EllipsizeChars("晋江文学", 4) == "晋江文学");
  assert(utf8EllipsizeChars("File", 4) == "File");
  assert(utf8EllipsizeChars("Files", 4) == "File…");
  assert(utf8EllipsizeChars("ABC", 4) == "ABC");
  assert(utf8EllipsizeChars("", 4).empty());
  assert(utf8EllipsizeChars(nullptr, 4).empty());

  assert(utf8EllipsizeChars("文件管理器", 4) == "文件管理…");
  assert(utf8EllipsizeChars("HelloWorld", 4) == "Hell…");
  assert(utf8EllipsizeChars("五个汉字名", 4) == "五个汉字…");

  printf("utf8 ellipsize PASS\n");
  return 0;
}

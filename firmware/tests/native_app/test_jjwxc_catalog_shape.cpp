#include "apps/M4xJsonStream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

class StringSink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    if (!data) return false;
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  }

  std::string body;
};

}  // namespace

int main() {
  // Regression for issue #17: JJWXC /androidapi/chapterList is an object
  // containing a `chapterlist` array, not a root array. This fixture locks the
  // provider adapter to that shape so a successful HTTP response cannot
  // silently regress to `catalog_empty` because the streaming extractor is
  // pointed at the root.
  const std::string json =
      R"JSON({"chapterlist":[{"chapterid":"101","chaptername":"第一章","chaptertype":"0","isvip":"0","islock":"0"},{"chapterid":"102","chaptername":"第二章","chaptertype":"0","isvip":"1","islock":"0"}]})JSON";

  StringSink sink;
  M4xJsonStream::RecordExtractor rows(
      {"chapterlist"},
      {"chapterid", "chaptername", "chaptertype", "isvip", "islock"},
      sink, 8);

  for (size_t off = 0; off < json.size();) {
    const size_t n = std::min<size_t>(11, json.size() - off);
    assert(rows.feed(reinterpret_cast<const uint8_t*>(json.data() + off), n));
    off += n;
  }

  assert(rows.finish());
  assert(rows.recordCount() == 2);
  assert(sink.body == "101\t第一章\t0\t0\t0\n102\t第二章\t0\t1\t0\n");

  std::cout << "jjwxc catalog shape test passed\n";
  return 0;
}

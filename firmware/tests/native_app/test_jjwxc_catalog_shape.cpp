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

class CountingSink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    if (!data) return false;
    bytes += len;
    for (size_t i = 0; i < len; ++i) {
      if (data[i] == '\n') ++rows;
    }
    return true;
  }

  size_t rows = 0;
  size_t bytes = 0;
};

void feedChunked(M4xJsonStream::RecordExtractor& rows, const std::string& json, size_t chunk) {
  for (size_t off = 0; off < json.size();) {
    const size_t n = std::min(chunk, json.size() - off);
    assert(rows.feed(reinterpret_cast<const uint8_t*>(json.data() + off), n));
    off += n;
  }
  assert(rows.finish());
}

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
  feedChunked(rows, json, 11);

  assert(rows.recordCount() == 2);
  assert(sink.body == "101\t第一章\t0\t0\t0\n102\t第二章\t0\t1\t0\n");

  // Large-book regression: the streaming parser must not have a hidden
  // few-hundred-chapter ceiling. Build a 5,000-row response and feed it in
  // small network-like chunks. The sink intentionally does not retain rows,
  // mirroring the SD FileRows path used on-device.
  constexpr size_t kLargeRows = 5000;
  std::string large = "{\"chapterlist\":[";
  large.reserve(kLargeRows * 96);
  for (size_t i = 0; i < kLargeRows; ++i) {
    if (i) large.push_back(',');
    large += "{\"chapterid\":\"" + std::to_string(100000 + i) +
             "\",\"chaptername\":\"chapter " + std::to_string(i + 1) +
             "\",\"chaptertype\":\"0\",\"isvip\":\"" + std::string((i % 7) == 0 ? "2" : "0") +
             "\",\"islock\":\"0\"}";
  }
  large += "]}";

  CountingSink largeSink;
  M4xJsonStream::RecordExtractor largeRows(
      {"chapterlist"},
      {"chapterid", "chaptername", "chaptertype", "isvip", "islock"},
      largeSink, kLargeRows + 1);
  feedChunked(largeRows, large, 257);

  assert(largeRows.recordCount() == kLargeRows);
  assert(largeSink.rows == kLargeRows);
  assert(largeSink.bytes > kLargeRows * 10);
  assert(largeRows.peakBufferedBytes() <= M4xJsonStream::RecordExtractor::kMaxBufferedBytes);

  std::cout << "jjwxc catalog shape/large-stream test passed\n";
  return 0;
}
#include "apps/M4xJsonStream.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {
class Sink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    if (!data) return false;
    bytes += len;
    for (size_t i = 0; i < len; ++i) {
      if (data[i] == '\n') ++rows;
    }
    return true;
  }
  size_t bytes = 0;
  size_t rows = 0;
};
}

int main(int argc, char** argv) {
  assert(argc == 2);
  std::ifstream in(argv[1], std::ios::binary);
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  assert(!body.empty());
  Sink sink;
  M4xJsonStream::RecordExtractor rows({"14000019"}, {"novelId", "novelName", "authorName", "_m4_progress"}, sink, 24);
  assert(rows.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  assert(rows.finish());
  assert(rows.recordCount() == 24);
  assert(sink.rows == 24);
  std::cout << "live JJWXC response parsed rows=" << rows.recordCount()
            << " bytes=" << sink.bytes << " peak=" << rows.peakBufferedBytes() << "\n";
}

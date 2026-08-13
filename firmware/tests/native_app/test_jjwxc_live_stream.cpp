#include "apps/M4xJsonStream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FileSink final : public M4xJsonStream::Sink {
 public:
  explicit FileSink(const std::string& path) : out_(path, std::ios::binary | std::ios::trunc) {}

  bool good() const { return out_.good(); }
  size_t bytes() const { return bytes_; }

  bool write(const uint8_t* data, size_t len) override {
    if (!data || !out_) return false;
    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!out_) return false;
    bytes_ += len;
    return true;
  }

 private:
  std::ofstream out_;
  size_t bytes_ = 0;
};

bool feedFile(const std::string& path, M4xJsonStream::RecordExtractor& extractor) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> buf(257);
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    if (n > 0 && !extractor.feed(buf.data(), static_cast<size_t>(n))) return false;
  }
  return extractor.finish();
}

bool feedFile(const std::string& path, M4xJsonStream::ScalarStreamExtractor& extractor) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> buf(257);
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    if (n > 0 && !extractor.feed(buf.data(), static_cast<size_t>(n))) return false;
  }
  return extractor.finish();
}

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> out;
  std::string field;
  std::istringstream ss(line);
  while (std::getline(ss, field, '\t')) out.push_back(field);
  return out;
}

std::string firstFreeChapter(const std::string& tsvPath, size_t& rowCount) {
  std::ifstream in(tsvPath, std::ios::binary);
  if (!in) return {};
  std::string line;
  std::string firstFree;
  rowCount = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    ++rowCount;
    const auto f = splitTabs(line);
    if (firstFree.empty() && f.size() >= 5 && !f[0].empty() && f[2] != "1" && f[3] == "0") {
      firstFree = f[0];
    }
  }
  return firstFree;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 5) {
    std::cerr << "usage: test_jjwxc_live_stream <catalog.json> <catalog.tsv> [chapter.json chapter.txt]\n";
    return 2;
  }

  FileSink catalogSink(argv[2]);
  if (!catalogSink.good()) {
    std::cerr << "catalog_sink_open_failed\n";
    return 3;
  }
  M4xJsonStream::RecordExtractor catalog(
      {"chapterlist"},
      {"chapterid", "chaptername", "chaptertype", "isvip", "islock"},
      catalogSink);
  if (!feedFile(argv[1], catalog)) {
    std::cerr << "catalog_parse_failed=" << M4xJsonStream::errorString(catalog.error()) << "\n";
    return 4;
  }

  size_t tsvRows = 0;
  const std::string freeUid = firstFreeChapter(argv[2], tsvRows);
  if (catalog.recordCount() != tsvRows || tsvRows < 100 || freeUid.empty()) {
    std::cerr << "catalog_validation_failed rows=" << tsvRows << " free_uid=" << (freeUid.empty() ? 0 : 1) << "\n";
    return 5;
  }
  if (catalog.peakBufferedBytes() > M4xJsonStream::RecordExtractor::kMaxBufferedBytes) {
    std::cerr << "catalog_buffer_bound_failed peak=" << catalog.peakBufferedBytes() << "\n";
    return 6;
  }

  std::cout << "catalog_rows=" << tsvRows << "\n";
  std::cout << "catalog_tsv_bytes=" << catalogSink.bytes() << "\n";
  std::cout << "catalog_peak_buffer=" << catalog.peakBufferedBytes() << "\n";
  std::cout << "first_free_uid=" << freeUid << "\n";

  if (argc == 5) {
    FileSink chapterSink(argv[4]);
    if (!chapterSink.good()) {
      std::cerr << "chapter_sink_open_failed\n";
      return 7;
    }
    M4xJsonStream::ScalarStreamExtractor chapter({}, "content", chapterSink);
    if (!feedFile(argv[3], chapter)) {
      std::cerr << "chapter_parse_failed=" << M4xJsonStream::errorString(chapter.error()) << "\n";
      return 8;
    }
    if (!chapter.fieldSeen() || chapter.bytesWritten() < 32 || chapterSink.bytes() != chapter.bytesWritten()) {
      std::cerr << "chapter_validation_failed bytes=" << chapter.bytesWritten() << "\n";
      return 9;
    }
    std::cout << "chapter_bytes=" << chapter.bytesWritten() << "\n";
  }

  return 0;
}

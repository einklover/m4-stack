// Multi-level host tests for shared progressive catalog policy + first-window
// streaming (all native plugins: legado / fanqie / jjwxc / weread).
//
// Levels:
//   L1 pure helpers (titles / rows / strategy)
//   L2 FirstWindowSink counting + cancel trip
//   L3 stream extract first window then full body (JSON fixture)

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4ProgressiveCatalog.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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

std::string makeChapterListJson(size_t n) {
  std::string j = R"({"data":[)";
  for (size_t i = 0; i < n; ++i) {
    if (i) j += ',';
    j += "{\"index\":";
    j += std::to_string(i);
    j += ",\"title\":\"章";
    j += std::to_string(i + 1);
    j += "\"}";
  }
  j += "]}";
  return j;
}

void level1_pure() {
  using namespace M4ProgressiveCatalog;
  assert(placeholderTitle(0) == "第1章");
  assert(placeholderTitle(9) == "第10章");
  assert(indexTitleRow(0, "开端") == "0\t开端\n");
  assert(indexTitleRow(2, "a\tb\nc") == "2\ta b c\n");

  const auto body = buildPlaceholderBody(3);
  assert(body == "0\t第1章\n1\t第2章\n2\t第3章\n");
  assert(buildPlaceholderBody(0).empty());

  assert(chooseStrategy(0, 64) == OpenStrategy::WindowThenFull);
  assert(chooseStrategy(2968, 64) == OpenStrategy::PlaceholderThenFull);
  assert(chooseStrategy(0, 0) == OpenStrategy::FullStream);
  assert(partialOk(1, true));
  assert(!partialOk(0, false));
  std::cout << "L1 pure helpers OK\n";
}

void level2_window_sink() {
  using namespace M4ProgressiveCatalog;
  StringSink inner;
  FirstWindowSink window(inner, 3);
  auto cancel = windowCancel(window, [] { return false; });
  assert(!cancel());
  assert(window.write(reinterpret_cast<const uint8_t*>("0\ta\n"), 4));
  assert(window.count() == 1);
  assert(!window.windowReady());
  assert(!cancel());
  assert(window.write(reinterpret_cast<const uint8_t*>("1\tb\n"), 4));
  assert(window.write(reinterpret_cast<const uint8_t*>("2\tc\n"), 4));
  assert(window.windowReady());
  assert(window.count() == 3);
  assert(cancel());  // first window trips cancel
  assert(inner.body == "0\ta\n1\tb\n2\tc\n");
  std::cout << "L2 FirstWindowSink OK\n";
}

void level3_stream_first_window() {
  using namespace M4ProgressiveCatalog;
  // 20-chapter Legado-shaped list; first window = 5 → cancel after 5 rows.
  const std::string json = makeChapterListJson(20);
  StringSink inner;
  FirstWindowSink window(inner, 5);
  M4xJsonStream::RecordExtractor rows({"data"}, {"index", "title"}, window, 200000);

  bool cancelled = false;
  size_t fed = 0;
  for (size_t off = 0; off < json.size() && !cancelled;) {
    const size_t n = std::min<size_t>(17, json.size() - off);
    if (!rows.feed(reinterpret_cast<const uint8_t*>(json.data() + off), n)) {
      // Window sink still returns true; cancel is separate. Feed fails only on
      // parse/sink error. Simulate caller cancel after window.
      break;
    }
    off += n;
    fed = off;
    if (window.windowReady()) {
      cancelled = true;  // stop feeding like HTTP cancel
      break;
    }
  }
  assert(window.windowReady());
  assert(window.count() == 5);
  assert(cancelled);
  // Partial TSV has first 5 real titles (not placeholders).
  assert(inner.body.find("0\t章1\n") != std::string::npos);
  assert(inner.body.find("4\t章5\n") != std::string::npos);
  assert(inner.body.find("5\t章6\n") == std::string::npos);
  assert(fed < json.size());  // did not need the whole body
  std::cout << "L3 first-window stream OK (fed " << fed << "/" << json.size() << " bytes)\n";
}

void level4_full_stream_small_catalog() {
  // Catalog smaller than firstWindow → full parse, no early cancel.
  const std::string json = makeChapterListJson(3);
  StringSink inner;
  M4ProgressiveCatalog::FirstWindowSink window(inner, 64);
  M4xJsonStream::RecordExtractor rows({"data"}, {"index", "title"}, window, 200000);
  assert(rows.feed(reinterpret_cast<const uint8_t*>(json.data()), json.size()));
  assert(rows.finish());
  assert(rows.recordCount() == 3);
  assert(!window.windowReady());
  assert(inner.body == "0\t章1\n1\t章2\n2\t章3\n");
  std::cout << "L4 small full stream OK\n";
}

void level5_placeholder_scale() {
  // 2968 chapters (real Legado long-novel scale) must stay tiny vs multi-MB JSON.
  const auto body = M4ProgressiveCatalog::buildPlaceholderBody(2968);
  assert(!body.empty());
  assert(body.size() < 80u * 1024u);  // << 2.5MB chapterList JSON
  // Count lines
  size_t lines = 0;
  for (char c : body)
    if (c == '\n') ++lines;
  assert(lines == 2968);
  std::cout << "L5 placeholder scale OK (2968 rows, " << body.size() << " bytes)\n";
}

}  // namespace

int main() {
  level1_pure();
  level2_window_sink();
  level3_stream_first_window();
  level4_full_stream_small_catalog();
  level5_placeholder_scale();
  std::cout << "progressive catalog multi-level tests passed\n";
  return 0;
}

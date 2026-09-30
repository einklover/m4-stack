#include "../../src/activities/boot_sleep/SleepImageScan.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using SleepImageScan::Step;
using SleepImageScan::Window;

static void expectSkip(const std::string& name, const char* suffix) {
  Window w;
  w.begin(0);
  assert(SleepImageScan::beginEntry(w, 0, false, name, suffix) == Step::Skip);
  assert(w.visited == 1);
  assert(!w.partial);
}

static void testShortNamesDoNotIndexOrThrow() {
  assert(SleepImageScan::nameSkipped(""));
  assert(SleepImageScan::nameSkipped("."));
  assert(SleepImageScan::nameSkipped(".a"));
  assert(!SleepImageScan::nameSkipped("a"));
  assert(!SleepImageScan::nameHasSuffix("", ".bmp"));
  assert(!SleepImageScan::nameHasSuffix("a", ".bmp"));
  assert(!SleepImageScan::nameHasSuffix("ab", ".bmp"));
  assert(!SleepImageScan::nameHasSuffix("abc", ".bmp"));
  assert(SleepImageScan::nameHasSuffix("a.bmp", ".bmp"));
  assert(SleepImageScan::nameHasSuffix("abc.pngtxt", ".pngtxt"));
  assert(!SleepImageScan::nameHasSuffix("pngtxt", ".pngtxt"));

  expectSkip("", ".bmp");
  expectSkip("a", ".bmp");
  expectSkip("abc", ".bmp");
  expectSkip(".bmp", ".bmp");

  Window dir;
  dir.begin(10);
  assert(SleepImageScan::beginEntry(dir, 10, true, "", ".bmp") == Step::Skip);

  Window ok;
  ok.begin(10);
  assert(SleepImageScan::beginEntry(ok, 10, false, "cover.bmp", ".bmp") == Step::Accept);
  assert(ok.visited == 1);
}

static void testBudgetStopsAndClosesTheInHandEntry() {
  Window w;
  w.begin(1000);
  std::vector<std::string> kept;
  struct Ent {
    bool dir;
    std::string name;
    uint32_t now;
    bool closed = false;
  };
  std::vector<Ent> ents;
  for (int i = 0; i < 40; ++i) ents.push_back({false, "note" + std::to_string(i) + ".txt", 1000});
  ents.push_back({true, "", 1000});
  ents.push_back({false, "", 1000});
  ents.push_back({false, "a", 1000});
  ents.push_back({false, "abc", 1000});
  ents.push_back({false, "ok.bmp", 1000});
  ents.push_back({false, "late.bmp", 1000 + SleepImageScan::kBudgetMs});

  for (auto& e : ents) {
    const Step step = SleepImageScan::beginEntry(w, e.now, e.dir, e.name, ".bmp");
    e.closed = true;
    if (step == Step::StopBudget) break;
    if (step == Step::Accept) kept.push_back(e.name);
  }
  assert(ents.back().closed);
  assert(kept.size() == 1);
  assert(kept[0] == "ok.bmp");
  assert(w.partial);
  assert(w.visited == ents.size() - 1);
}

static void testEntryCapAndWrap() {
  Window w;
  w.begin(0xfffffff0u);
  int accepted = 0;
  bool stopped = false;
  for (uint32_t i = 0; i < SleepImageScan::kMaxEntries + 3; ++i) {
    const uint32_t now = 0xfffffff0u + i;  // still inside 400ms
    const Step step = SleepImageScan::beginEntry(w, now, false, "f.bmp", ".bmp");
    if (step == Step::StopBudget) {
      stopped = true;
      break;
    }
    assert(step == Step::Accept);
    ++accepted;
  }
  assert(stopped);
  assert(accepted == static_cast<int>(SleepImageScan::kMaxEntries));
  assert(w.partial);

  Window wrapped;
  wrapped.begin(0xfffffff0u);
  const Step late = SleepImageScan::beginEntry(wrapped, 0xfffffff0u + SleepImageScan::kBudgetMs, false,
                                                "x.bmp", ".bmp");
  assert(late == Step::StopBudget);
  assert(wrapped.visited == 0);
}

static void testNoCandidateAndPngFilterStaysOutsideSuffix() {
  Window w;
  w.begin(0);
  assert(SleepImageScan::beginEntry(w, 0, false, "readme", nullptr) == Step::Accept);
  assert(SleepImageScan::beginEntry(w, 1, false, "", nullptr) == Step::Skip);
  assert(!w.partial);
}

int main() {
  testShortNamesDoNotIndexOrThrow();
  testBudgetStopsAndClosesTheInHandEntry();
  testEntryCapAndWrap();
  testNoCandidateAndPngFilterStaysOutsideSuffix();
  std::cout << "test_sleep_image_scan ok\n";
  return 0;
}

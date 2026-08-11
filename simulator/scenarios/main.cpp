// M4Sim scenario runner. Usage:
//   m4_simulator                        determinism + multi-seed fuzz suite
//   m4_simulator --list                 list scenario names
//   m4_simulator <name>                 run one scenario (seed 0x5eed)
//   m4_simulator --seed N <name>        run with a specific schedule seed
//   m4_simulator --seeds 1:200 [name]   schedule fuzz over a seed range
// Exit code 0 = all invariants held.
//
// Determinism is NOT "two seeds give the same failure vector". Two checks:
//   1. DETERMINISTIC REPLAY — same seed run twice → identical timeline hash.
//   2. SCHEDULE FUZZ — many seeds may produce different timelines, but every
//      invariant (eventually/never/after + expectFail) must hold in all.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/SimKernel.h"
#include "scenarios/scenarios.h"

using namespace m4sim;

namespace {
struct RunResult {
  std::vector<std::string> failures;
  uint64_t hash = 0;
};

RunResult runScenario(const Scenario& s, uint32_t seed) {
  SimTrace trace;
  RunResult r;
  r.failures = s.run(seed, &trace);
  r.hash = trace.hash();
  return r;
}

bool invariantPass(const Scenario& s, const RunResult& r) {
  return r.failures.empty() == !s.expectFail;
}
}  // namespace

int main(int argc, char** argv) {
  uint32_t seed = 0x5eed;
  uint32_t fuzzSeeds = 0;  // 0 = default suite (below), else run fuzz
  std::string filter;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--list") {
      for (auto& s : allScenarios())
        printf("%s%s\n", s.name.c_str(), s.expectFail ? " (expect regression)" : "");
      return 0;
    } else if (a == "--seed" && i + 1 < argc) {
      seed = (uint32_t)strtoul(argv[++i], nullptr, 0);
    } else if (a == "--seeds" && i + 1 < argc) {
      // e.g. "1:200" — schedule fuzz across a seed range
      const char* spec = argv[++i];
      uint32_t from = 1, to = 200;
      if (sscanf(spec, "%u:%u", &from, &to) != 2) {
        from = 1;
        to = (uint32_t)strtoul(spec, nullptr, 0);
      }
      seed = from;
      fuzzSeeds = to - from + 1;
    } else if (a == "-v" || a == "--verbose") {
      verbose = true;
    } else if (a == "--help" || a == "-h") {
      printf("m4_simulator [--seed N] [--seeds from:to] [-v] [--list] [<scenario-name>]\n");
      return 0;
    } else {
      filter = a;
    }
  }

  auto scens = allScenarios();

  // ── Schedule fuzz: invariants must hold across every seed ────────────
  // Checked BEFORE the single-filter path, so `--seeds 1:200 <name>` actually
  // fuzzes the named scenario (the old order silently ran the single path).
  if (fuzzSeeds > 0) {
    int fails = 0;
    for (auto& s : scens) {
      if (!filter.empty() && s.name != filter) continue;
      for (uint32_t sd = seed; sd < seed + fuzzSeeds; ++sd) {
        auto r = runScenario(s, sd);
        if (!invariantPass(s, r)) {
          fails++;
          printf("[FAIL] %s seed=%u\n", s.name.c_str(), sd);
          for (auto& f : r.failures) printf("    - %s\n", f.c_str());
          break;  // report first failing seed per scenario
        }
      }
    }
    printf("\n%zu scenarios × %u seeds fuzzed, %d failed\n", scens.size(), fuzzSeeds, fails);
    return fails == 0 ? 0 : 1;
  }

  // ── Single scenario run ──────────────────────────────────────────────
  if (!filter.empty()) {
    bool found = false;
    for (auto& s : scens) {
      if (s.name == filter) {
        found = true;
        auto r = runScenario(s, seed);
        bool pass = invariantPass(s, r);
        printf("[%s] %s (seed=%u%s)\n", pass ? "PASS" : "FAIL", s.name.c_str(), seed,
               s.expectFail ? ", expect regression" : "");
        if (verbose || !r.failures.empty()) {
          if (!r.failures.empty())
            for (auto& f : r.failures) printf("    - %s\n", f.c_str());
          SimTrace trace;
          s.run(seed, &trace);
          printf("  event timeline:\n%s\n", trace.renderTimeline().c_str());
        }
        return pass ? 0 : 1;
      }
    }
    if (!found) {
      fprintf(stderr, "unknown scenario '%s' (use --list)\n", filter.c_str());
      return 2;
    }
  }

  // ── Default suite: deterministic replay (same seed twice) + 2-seed fuzz ─
  int fails = 0;
  for (auto& s : scens) {
    auto r1 = runScenario(s, 1);
    auto r2 = runScenario(s, 1);   // same seed again
    auto r3 = runScenario(s, 2);   // different seed: timeline may differ
    bool replayOk = (r1.hash == r2.hash);
    bool inv1 = invariantPass(s, r1);
    bool inv2 = invariantPass(s, r3);
    bool pass = replayOk && inv1 && inv2;
    if (!pass) fails++;
    printf("[%s] %s%s%s%s\n", pass ? "PASS" : "FAIL", s.name.c_str(),
           s.expectFail ? " (expect regression)" : "",
           replayOk ? "" : " [NON-DETERMINISTIC REPLAY]",
           inv1 && inv2 ? "" : " [INVARIANT FAIL]");
    if (!r1.failures.empty()) {
      for (auto& f : r1.failures) printf("    - %s\n", f.c_str());
      if (verbose) {
        SimTrace trace;
        s.run(1, &trace);
        printf("%s\n", trace.renderTimeline().c_str());
      }
    }
  }
  printf("\n%zu/%zu passed (seed-1 replay hash deterministic + seed-2 fuzz)\n",
         scens.size() - fails, scens.size());
  return fails == 0 ? 0 : 1;
}

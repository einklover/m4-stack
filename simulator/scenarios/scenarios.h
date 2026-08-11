// M4Sim scenarios: the historical Murphy M4 bug classes as deterministic
// regression tests. Each scenario runs on the real ReaderModel / SimHeap /
// SimPanel / chunked-fixture, in virtual time, and asserts temporal invariants.
//
// Two kinds of scenarios:
//   - "PASS expected": correct firmware + assertion must hold.
//   - "FAIL expected": bug knob flipped; the assertion MUST fire (proves the
//     simulator catches the regression — this is the CI value).
#pragma once

#include <string>
#include <vector>

#include "core/SimKernel.h"

namespace m4sim {

struct Scenario {
  std::string name;
  // Returns assertion failures. Empty = PASS.
  std::vector<std::string> (*run)(uint32_t seed, SimTrace* traceOut);
  bool expectFail;  // true: a failure here is a PASS (regression was caught)
};

std::vector<Scenario> allScenarios();

}  // namespace m4sim

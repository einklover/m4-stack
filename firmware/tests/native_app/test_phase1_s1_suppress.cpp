// Phase1 S1 RED: consume-once suppress-state gates.
// Depends on planned helper `util/M4SuppressState.h` which does not exist yet,
// so this translation unit must fail to compile until GREEN.
#include "util/M4SuppressState.h"

#include <cassert>
#include <cstdio>

// Planned contract (GREEN must provide these four APIs):
//   void m4SuppressNextRelease(int buttonIndex);
//   bool m4ConsumeSuppressedRelease(int buttonIndex);
//   bool m4LongPressFired(int buttonIndex);
//   int  m4SuppressButtonIndex();  // currently armed index, or -1 when idle.

int main() {
  // fresh-press: nothing armed, nothing to consume.
  assert(m4SuppressButtonIndex() == -1);
  assert(!m4LongPressFired(0));
  assert(!m4ConsumeSuppressedRelease(0));

  // consume-once: first consume takes it, second sees idle.
  m4SuppressNextRelease(1);
  assert(m4SuppressButtonIndex() == 1);
  assert(m4ConsumeSuppressedRelease(1));
  assert(!m4ConsumeSuppressedRelease(1));
  assert(m4SuppressButtonIndex() == -1);

  // mixed-frame: arming one button must not suppress a different button.
  m4SuppressNextRelease(2);
  assert(!m4ConsumeSuppressedRelease(3));
  assert(m4SuppressButtonIndex() == 2);
  assert(m4ConsumeSuppressedRelease(2));
  assert(m4SuppressButtonIndex() == -1);

  // held-level: armed press reports long-press fired until its release is consumed.
  m4SuppressNextRelease(0);
  assert(m4LongPressFired(0));
  assert(m4SuppressButtonIndex() == 0);
  assert(m4ConsumeSuppressedRelease(0));
  assert(m4SuppressButtonIndex() == -1);

  std::printf("phase1-s1 suppress RED: all gates observe planned API\n");
  return 0;
}

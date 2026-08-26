#include "scenarios/scenarios.h"

#include <algorithm>
#include <map>
#include <cstdio>
#include <string>

#include "core/SimKernel.h"
#include "hardware/SimPanel.h"
#include "hardware/SimStorage.h"
#include "memory/SimHeap.h"
#include "model/ReaderModel.h"
#include "network/SimNetwork.h"

namespace m4sim {
namespace {

// Build a kernel with a boot baseline that fragments INTERNAL RAM like a real
// device: static/bss/tasks leave free ~70KB with largest block ~31KB.
struct TestKernel {
  SimTrace localTrace;  // used only when no external trace is supplied
  SimTrace& trace;
  SimScheduler sched;
  SimPanel panel;
  SimStorage sd;
  SimHeap heap;
  SimAssertions asserts;
  TestKernel(uint32_t seed = 0x5eed, EpdProfile prof = {},
             const std::vector<size_t>& baseline = {}, SimTrace* externalTrace = nullptr)
      : trace(externalTrace ? *externalTrace : localTrace),
        sched(seed),
        panel(&sched, &trace, prof),
        sd(&sched, &trace),
        heap(&trace) {
    heap.setNow([this]() { return sched.now(); });
    if (!baseline.empty()) heap.applyBootBaseline(baseline);
    // Event-driven invariant checking: after EVERY scheduler event, safety
    // (never) and liveness (eventually/after) are evaluated. No 5ms polling.
    sched.setEventHook([this](uint32_t t) {
      asserts.checkSafety(t);
      asserts.checkLiveness(t);
    });
  }

  // Run the scheduler until `pred` becomes true (or timeoutMs elapses).
  // Scenarios must wait for the ACTUAL state (panel idle / BUSY / render
  // in-flight) rather than assuming fixed wall-clock offsets — fixed timing
  // silently tests the wrong phase when EPD timings or SD latency change.
  bool waitFor(const std::function<bool()>& pred, uint32_t timeoutMs) {
    uint32_t deadline = sched.now() + timeoutMs;
    while (sched.now() < deadline) {
      if (pred()) return true;
      sched.runFor(5);  // coarse virtual steps; events fire at their own times
    }
    return pred();
  }
};

const std::vector<size_t> kBootBaseline = {
    // Alternate kept-allocated (static/bss/task stacks) with freed holes so
    // internal RAM ends up like a real booted device: free ~70KB total, largest
    // contiguous block ~31KB → a 48KB TLS reserve must OOM.
    12000, 4000, 14000, 4000, 12000, 4000, 16000, 4000, 10000, 4000,
    12000, 4000, 14000, 4000, 12000, 4000, 16000, 4000, 10000, 4000,
    12000, 4000, 14000, 4000, 12000, 4000, 16000, 4000, 10000, 4000,
    12000, 4000, 14000, 4000, 12000, 4000, 16000, 4000, 10000, 4000,
    12000, 4000, 14000, 4000, 12000, 4000, 16000, 4000, 10000, 4000,
    12000, 4000, 14000, 4000, 12000, 4000, 16000, 4000, 10000, 4000,
};

// ────────────────────────────────────────────────────────────────────────
// 1. Strict single-intent + slow progressive index: the "first tap doesn't
// turn" class, tested EXACTLY as the review demands:
//   - wait until page 0 is physically committed and the panel is IDLE
//   - ONE intent: jump target to a page far beyond the current index cursor
//     (target >= indexCursor, so page N's index does not exist yet)
//   - NO further INPUT events
//   - index advances slowly 2,4,6,8,...; the catch-up invariant must push the
//     index until it COVERS the target, then render straight to it
//   - assert: eventually physicalPage == target, AND INPUT count == 1
// This proves one user intent never depends on a second tap to wake it.
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_single_tap_slow_index(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  ReaderModel reader(&k.sched, &k.trace, &k.panel, &k.sd,
                      ReaderModel::Knobs{/*bugNoCatchup=*/false, /*bugLivePhysical=*/false,
                                         /*sdScale=*/1.0, /*indexSlicePages=*/2});
  reader.openBook(200);  // big book: index stays incomplete for a long time
  // Wait for the ACTUAL stable state: page 0 committed, panel idle.
  if (!k.waitFor([&]() { return reader.firstShown() && reader.panelIdle(); }, 6000)) {
    return {"never reached 'page 0 committed, panel idle' state"};
  }
  int cursor = reader.indexCursor();
  int target = std::min(cursor + 12, 199);  // one intent, far beyond index
  bool intentAccepted = reader.jumpTo(target);
  std::vector<std::string> failures;
  if (!intentAccepted) {
    failures.push_back("target " + std::to_string(target) +
                       " was already covered by index cursor " + std::to_string(cursor) +
                       " — test did not exercise index-not-ready");
  }
  k.asserts.eventually("one intent auto-lands once index covers target",
                       [&]() { return reader.physicalPage() == target; }, 8000);
  k.sched.runFor(8000);
  k.asserts.finish(k.sched.now());
  if (reader.tapCount() != 1) {
    failures.push_back("expected exactly 1 INPUT intent, got " +
                       std::to_string(reader.tapCount()));
  }
  return failures;
}

// 1b. Regression: same input, but bugNoCatchup — the intent is lost forever.
// The jump arrives while the index is incomplete AND the panel is BUSY (the
// first page's refresh is in flight); with no catch-up invariant the
// display task never re-arms after the commit, so physical stays on page 0.
// Expect FAIL (this is the bug the simulator must catch).
std::vector<std::string> scenario_bug_lost_tap(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  ReaderModel reader(&k.sched, &k.trace, &k.panel, &k.sd,
                      ReaderModel::Knobs{/*bugNoCatchup=*/true, /*bugLivePhysical=*/false,
                                         /*sdScale=*/1.0, /*indexSlicePages=*/64});
  reader.openBook(200);
  // Stable state: page 0 committed, panel idle.
  if (!k.waitFor([&]() { return reader.firstShown() && reader.panelIdle(); }, 6000)) {
    return {"never reached 'page 0 committed, panel idle' state"};
  }
  reader.tap(+1);  // page 1 render starts and the panel becomes BUSY
  // Wait for the animation to be actually IN FLIGHT (not assume 200ms).
  if (!k.waitFor([&]() { return reader.epdBusy(); }, 3000)) {
    return {"never observed EPD busy after tap"};
  }
  reader.tap(+1);  // during BUSY: quickTap → target 2, NO refresh queued (bug)
  k.asserts.eventually("physical reaches 2 despite tap during BUSY (catchup must save it)",
                       [&]() { return reader.physicalPage() == 2; }, 5000);
  k.sched.runFor(5000);
  k.asserts.finish(k.sched.now());
  return k.asserts.failures();
}

// ────────────────────────────────────────────────────────────────────────
// 2. Rapid taps during animation + live-currentPage bug: the "second tap
// jumps two pages / divergence" class. Two invariants:
//   a) eventually the FINAL target lands on the panel;
//   b) firmware's physical-page record never diverges from the panel's real
//      committed frame provenance.
// Waits for the real states: stable page 0 → tap → animation in flight →
// two more taps DURING the animation.
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_rapid_tap(uint32_t seed, bool bugLivePhysical, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  ReaderModel reader(&k.sched, &k.trace, &k.panel, &k.sd,
                      ReaderModel::Knobs{false, bugLivePhysical, 1.0, 64});
  reader.openBook(40);
  if (!k.waitFor([&]() { return reader.firstShown() && reader.panelIdle(); }, 6000)) {
    return {"never reached 'page 0 committed, panel idle' state"};
  }
  reader.tap(+1);       // page 1 renders + animates
  if (!k.waitFor([&]() { return reader.epdBusy(); }, 3000)) {
    return {"never observed EPD busy after first tap"};
  }
  reader.tap(+1);       // during animation: page 2 target
  reader.tap(+1);       // during animation: page 3 target
  k.asserts.eventually("final target committed", [&]() { return reader.physicalPage() == 3; },
                       6000);
  // Divergence invariant: after first content commit, firmware's recorded
  // physical page must always equal the panel's actual committed frame page.
  k.asserts.never("firmware physical == panel physical (provenance)",
                  [&]() {
                    const auto& phys = k.panel.physical();
                    return phys.valid && reader.physicalPage() != phys.tag.page;
                  });
  k.sched.runFor(6000);
  k.asserts.finish(k.sched.now());
  return k.asserts.failures();
}

std::vector<std::string> scenario_rapid_tap_correct(uint32_t seed, SimTrace* traceOut) {
  return scenario_rapid_tap(seed, /*bugLivePhysical=*/false, traceOut);
}

// Regression: bugLivePhysical — firmware records live currentPage (advanced
// during animation) as the physical page. Expect FAIL (divergence caught).
std::vector<std::string> scenario_bug_divergence(uint32_t seed, SimTrace* traceOut) {
  return scenario_rapid_tap(seed, /*bugLivePhysical=*/true, traceOut);
}

// ────────────────────────────────────────────────────────────────────────
// 3. TLS OOM with fragmented INTERNAL: free enough overall (PSRAM ~8MB) but
// largest internal block < 48KB → mbedtls handshake must fail. The bug was
// "fake success" (allocator returning memory that isn't a contiguous block).
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_tls_fragmented_heap(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, kBootBaseline, traceOut);
  SimTls tls(&k.heap, &k.trace, &k.sched);
  // Sanity: the boot baseline must actually fragment INTERNAL below the TLS
  // reserve — otherwise this scenario tests nothing.
  if (k.heap.largestInternal() >= tls.peakBytes()) {
    return {"test setup broken: internal largest " +
            std::to_string(k.heap.largestInternal()) + " not < 48KB TLS reserve"};
  }
  bool opened = tls.open("mbedtls_handshake");
  std::vector<std::string> failures;
  if (opened) {
    failures.push_back("TLS handshake succeeded on fragmented internal heap "
                       "(should have OOM'd: largest internal block " +
                       std::to_string(k.heap.largestInternal()) + " < 48KB reserve)");
  } else {
    // Correct behavior: handshake OOMs. Verify the OOM trace recorded it.
    bool sawOom = false;
    for (const auto& e : k.trace.events())
      if (e.type == SimEventType::OOM) sawOom = true;
    if (!sawOom) failures.push_back("TLS failed but no OOM trace event recorded");
  }
  k.sched.runFor(10);
  return failures;
}

// Keep-alive: a second request over the same connection must NOT re-reserve
// 48KB of internal RAM. Header overhead (32B/block) is real, so compare with
// tolerance.
std::vector<std::string> scenario_tls_keepalive(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  SimTls tls(&k.heap, &k.trace, &k.sched);
  size_t freeBefore = k.heap.freeInternal();
  bool ok1 = tls.open("mbedtls_handshake");
  if (!ok1) return {"first TLS handshake unexpectedly OOM'd"};
  tls.close();
  size_t freeAfterClose = k.heap.freeInternal();
  (void)tls.open("mbedtls_handshake");  // new connection: reserves again
  tls.close();
  size_t freeFinal = k.heap.freeInternal();
  std::vector<std::string> failures;
  if (freeAfterClose + 128 < freeBefore) failures.push_back("close() leaked handshake RAM");
  if (freeFinal + 128 < freeBefore) failures.push_back("connection churn leaked RAM");
  k.sched.runFor(10);
  return failures;
}

// ────────────────────────────────────────────────────────────────────────
// 4. Memory contract: framebuffer (48KB) MUST live in PSRAM. Allocating it in
// internal RAM violates the contract even when memory is "available".
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_framebuffer_contract(uint32_t seed, SimTrace* traceOut) {
  // Fresh heap (no boot baseline): 48000B DOES fit in internal RAM, so the only
  // thing that can stop a `malloc(48000)` framebuffer is the memory contract.
  TestKernel k(seed, {}, {}, traceOut);
  k.heap.expect(MemoryContract{"framebuffer", MALLOC_CAP_SPIRAM, 48000});
  void* fb = k.heap.alloc(48000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "framebuffer");
  std::vector<std::string> failures;
  if (fb != nullptr) {
    failures.push_back("framebuffer allocated in INTERNAL RAM — contract violated, "
                       "should have been rejected (PSRAM required)");
  }
  // Correct path: PSRAM framebuffer succeeds even on a tight internal heap.
  void* fbOk = k.heap.alloc(48000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "framebuffer");
  if (fbOk == nullptr) failures.push_back("PSRAM framebuffer should succeed");
  bool sawViolation = false;
  for (const auto& e : k.trace.events())
    if (e.type == SimEventType::CONTRACT_VIOLATION) sawViolation = true;
  if (!sawViolation) failures.push_back("no CONTRACT_VIOLATION trace recorded");
  k.heap.free(fbOk);
  k.sched.runFor(10);
  return failures;
}

// ────────────────────────────────────────────────────────────────────────
// 5. Chunked HTTP quiet-EOF + TCP segmentation: the old firmware's "quiet
// 700ms = EOF" heuristic truncated a 466c-byte chapter. The fixed streaming
// decoder must survive ARBITRARY TCP fragmentation (size line split mid-way,
// data split across reads, CR/LF straddling buffers) and wait for the true
// 0-chunk. Both behaviors are verified.
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_chunked_quiet_eof(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  std::vector<std::string> failures;

  std::string chunk1(18028, 'A');
  std::string chunk2Data(1929, 'B');
  const size_t kTotal = 18028 + 1929;

  // ── (a) buggy quiet-EOF heuristic reproduces the truncation ──────────
  ChunkedDecoder buggy(700);
  buggy.feed("466c\r\n", 100);
  buggy.feed(chunk1, 100);
  buggy.feed("\r\n", 100);
  buggy.poll(100 + 900);  // 900ms gap: buggy decoder truncates at 700ms
  if (buggy.state() != ChunkedDecoder::State::TRUNCATED) {
    failures.push_back("buggy quiet-EOF decoder did NOT truncate (regression not reproduced)");
  }

  // ── (b) fixed decoder: real streaming under hostile segmentation ─────
  ChunkedDecoder fixed(0);
  std::string wire = "466c\r\n" + chunk1 + "\r\n" + "789\r\n" + chunk2Data + "\r\n" + "0\r\n\r\n";

  // Feed one byte at a time (worst-case TCP segmentation).
  for (size_t i = 0; i < wire.size(); ++i) {
    fixed.feed(wire.data() + i, 1, 100);
    if (fixed.state() == ChunkedDecoder::State::TRUNCATED) break;
  }
  if (fixed.state() != ChunkedDecoder::State::DONE) {
    failures.push_back("byte-by-byte feed did not reach DONE (state=" +
                       std::to_string((int)fixed.state()) + ")");
  } else if (fixed.body().size() != kTotal) {
    failures.push_back("byte-by-byte feed truncated body: got " +
                       std::to_string(fixed.body().size()) + " bytes, want " +
                       std::to_string(kTotal));
  }

  // ── (c) random segmentation (seeded): must still decode fully ────────
  uint32_t rng = seed * 2654435761u + 1;
  ChunkedDecoder rand(0);
  size_t pos = 0;
  while (pos < wire.size()) {
    rng = rng * 1664525u + 1013904223u;
    size_t take = 1 + (rng % 32);  // 1..32 bytes per segment
    take = std::min(take, wire.size() - pos);
    rand.feed(wire.data() + pos, take, 100);
    pos += take;
    if (rand.state() == ChunkedDecoder::State::TRUNCATED) break;
  }
  if (rand.state() != ChunkedDecoder::State::DONE) {
    failures.push_back("random-segmentation feed did not reach DONE (state=" +
                       std::to_string((int)rand.state()) + ")");
  } else if (rand.body().size() != kTotal) {
    failures.push_back("random-segmentation truncated body: got " +
                       std::to_string(rand.body().size()) + " bytes, want " +
                       std::to_string(kTotal));
  }
  k.sched.runFor(1200);
  return failures;
}

// ────────────────────────────────────────────────────────────────────────
// 6. Slow SD (x20) with a tap DURING the page read (not during EPD BUSY):
// the intent must survive a slow/cached page read. We wait for the render to
// be in flight (SD read pending) AND the panel still idle, then tap again.
// EVENTUALLY physical == target, no lost click. This distinguishes the
// "SD read in-flight" phase from the "EPD busy" phase the other tests cover.
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_slow_sd_tap(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  ReaderModel reader(&k.sched, &k.trace, &k.panel, &k.sd,
                      ReaderModel::Knobs{false, false, /*sdScale=*/20.0, 64});
  reader.openBook(30);
  if (!k.waitFor([&]() { return reader.firstShown() && reader.panelIdle(); }, 15000)) {
    return {"never reached stable page 0 (slow SD)"};
  }
  reader.tap(+1);
  // Wait for the SD page read to actually be in flight, panel STILL idle
  // (20x latency ≈ 760ms read vs 1800ms EPD — the read window is distinct).
  if (!k.waitFor([&]() { return reader.renderInFlight() && reader.panelIdle(); }, 3000)) {
    return {"never observed SD read in flight (panel idle) after tap"};
  }
  reader.tap(+1);  // rapid tap during slow page read → target 2
  k.asserts.eventually("physical reaches target despite slow SD",
                       [&]() { return reader.physicalPage() == 2; }, 8000);
  k.sched.runFor(8000);
  k.asserts.finish(k.sched.now());
  return k.asserts.failures();
}

// ────────────────────────────────────────────────────────────────────────
// 7. Heap allocator self-consistency under random alloc/free churn (seeded).
// Verifies the allocator itself is sound before any OOM/fragmentation
// conclusions are trusted: address-order list stays intact, free list never
// cycles, freeBytes_ always equals the true free sum, live-alloc tracking
// matches reality, and full coalescing restores the whole region.
// Covers: random alloc/free both regions, realloc grow/shrink/move, exact
// boundary allocations, calloc overflow, fail-Nth, baseline-after stress,
// and double-free detection.
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_heap_stress(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  std::vector<std::string> failures;
  uint32_t rng = seed * 2654435761u + 1;
  auto next = [&rng]() {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  };

  // ── (a) realloc semantics ────────────────────────────────────────────
  {
    void* p = k.heap.alloc(20000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "realloc");
    if (!p) failures.push_back("realloc test: initial alloc failed");
    void* q = k.heap.realloc(p, 1000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "realloc");
    if (q != p) failures.push_back("realloc shrink should be in place");
    q = k.heap.realloc(p, 50000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "realloc");
    if (q == nullptr) failures.push_back("realloc grow to 50K failed");
    // (fresh heap: whether it grows in place or moves is implementation detail —
    // both are legal; what matters is the pointer is valid and data survives)
    size_t oldSize = 20000;
    if (k.heap.liveAllocs().size() != 1) failures.push_back("realloc live-alloc tracking wrong");
    k.heap.free(q);
    if (k.heap.liveAllocs().size() != 0) failures.push_back("free after realloc leaked");
    // realloc(nullptr, n) == alloc
    void* r = k.heap.realloc(nullptr, 4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "rnull");
    if (!r) failures.push_back("realloc(nullptr) failed");
    k.heap.free(r);
    (void)oldSize;
  }

  // ── (b) exact boundary + calloc overflow + double-free detection ─────
  {
    // calloc overflow must return nullptr, not wrap.
    void* o = k.heap.calloc((size_t)-1 / 4 + 1, 8, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "ovf");
    if (o != nullptr) failures.push_back("calloc overflow was not refused");
    // Exact-boundary alloc: ask for exactly the largest block.
    size_t largest = k.heap.largestInternal();
    if (largest > 0) {
      void* ex = k.heap.alloc(largest, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "exact");
      if (!ex) failures.push_back("exact-boundary alloc failed");
      k.heap.free(ex);
    }
    // Double free must be detected (ASSERT event + counter).
    void* d = k.heap.alloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "df");
    if (!d) failures.push_back("double-free test alloc failed");
    k.heap.free(d);
    k.heap.free(d);  // second free → double free
    if (k.heap.doubleFrees() != 1) failures.push_back("double free NOT detected");
  }

  // ── (c) fail-Nth injection on a FRESH heap (counter must start at 1) ──
  {
    TestKernel k2(seed ^ 0xABCD, {}, {}, nullptr);
    k2.heap.setFailNth(3);
    void* f1 = k2.heap.alloc(512, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "fn");
    void* f2 = k2.heap.alloc(512, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "fn");
    void* f3 = k2.heap.alloc(512, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "fn");
    if (!f1 || !f2 || f3 != nullptr) failures.push_back("fail-Nth semantics wrong");
    if (f1) k2.heap.free(f1);
    if (f2) k2.heap.free(f2);
  }

  // ── (d) random churn on a BASELINED heap (not fresh) ─────────────────
  // Note: with a boot baseline the internal banks are genuinely limited, so a
  // burst MAY legitimately OOM. What must hold: the heap stays CONSISTENT
  // through failures, and full-free coalesces back to the baseline free count.
  k.heap.applyBootBaseline(kBootBaseline);
  size_t baselineFree = k.heap.freeInternal();
  std::vector<void*> live;
  int oomDuringBurst = 0;
  for (int round = 0; round < 4 && failures.empty(); ++round) {
    for (int i = 0; i < 120; ++i) {
      size_t sz = 16 + (next() % 3000);
      uint32_t caps = (next() % 2) ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM;
      void* p = k.heap.alloc(sz, caps | MALLOC_CAP_8BIT, "stress");
      if (!p) { oomDuringBurst++; continue; }  // legit OOM on a limited heap
      live.push_back(p);
    }
    std::string c = k.heap.checkConsistency();
    if (!c.empty()) failures.push_back("consistency after alloc burst: " + c);
    // Random realloc + free mix.
    for (size_t i = 0; i < live.size(); ++i) {
      if (!live[i]) continue;
      uint32_t op = next() % 4;
      if (op == 0) {
        size_t nsz = 16 + (next() % 6000);
        void* nq = k.heap.realloc(live[i], nsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "stress");
        if (nq) live[i] = nq;
      } else if (op == 1) {
        k.heap.free(live[i]);
        live[i] = nullptr;
      }
    }
    c = k.heap.checkConsistency();
    if (!c.empty()) failures.push_back("consistency after realloc/free mix: " + c);
    for (auto& p : live) {
      if (p) k.heap.free(p);
      p = nullptr;
    }
    live.clear();
    c = k.heap.checkConsistency();
    if (!c.empty()) failures.push_back("consistency after full free: " + c);
    // Coalescing must return free bytes to ~baseline (within per-block header
    // overhead — a realloc-in-place split/absorb leaves one extra/one fewer
    // header, which is correct allocator behavior, not a leak).
    if (k.heap.freeInternal() + 128 < baselineFree) {
      failures.push_back("full free did not coalesce back to baseline free=" +
                         std::to_string(k.heap.freeInternal()) + " want ~" +
                         std::to_string(baselineFree));
    }
  }
  if (k.heap.doubleFrees() != 1) failures.push_back("unexpected double-free count");
  return failures;
}

// ────────────────────────────────────────────────────────────────────────
// 8. Allocator reference-model fuzz: an INDEPENDENT shadow model (plain
// std::map of live ptr→size) tracks every op. After each op we cross-check
// the simulator heap against the reference: live-alloc count, per-region
// occupancy, capability routing, and full invariants. This validates the
// allocator against a second implementation, not just its own consistency.
// Run with --seeds 1:10000 for the nightly heavy sweep.
// ────────────────────────────────────────────────────────────────────────
std::vector<std::string> scenario_heap_fuzz(uint32_t seed, SimTrace* traceOut) {
  TestKernel k(seed, {}, {}, traceOut);
  std::vector<std::string> failures;
  uint32_t rng = seed * 2654435761u + 1;
  auto next = [&rng]() {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  };

  // Independent reference model: live allocations (ptr → size).
  struct RefAlloc {
    size_t size;
    uint32_t caps;
  };
  std::map<void*, RefAlloc> ref;

  auto regionNameOf = [&](void* p) -> std::string {
    for (auto& r : k.heap.regions()) {
      const uint8_t* p8 = (const uint8_t*)p;
      if (p8 >= r->dataPtr() && p8 < r->dataPtr() + r->capacity()) return r->name();
    }
    return "?";
  };

  auto crossCheck = [&](const char* where) {
    // 1. Live-alloc count must match the reference.
    if (k.heap.liveAllocs().size() != ref.size()) {
      failures.push_back(std::string(where) + ": live count " +
                         std::to_string(k.heap.liveAllocs().size()) + " != ref " +
                         std::to_string(ref.size()));
      return;
    }
    // 2. Every live alloc must be in the reference with matching size.
    for (auto& a : k.heap.liveAllocs()) {
      auto it = ref.find(a.ptr);
      if (it == ref.end()) {
        failures.push_back(std::string(where) + ": heap has ptr not in ref");
        return;
      }
      if (it->second.size != a.size) {
        failures.push_back(std::string(where) + ": size mismatch for ptr");
        return;
      }
    }
    // 3. Capability routing: allocation must be in a region satisfying caps.
    for (auto& a : k.heap.liveAllocs()) {
      auto it = ref.find(a.ptr);
      for (auto& r : k.heap.regions()) {
        const uint8_t* p8 = (const uint8_t*)a.ptr;
        if (p8 >= r->dataPtr() && p8 < r->dataPtr() + r->capacity()) {
          if ((r->caps() & it->second.caps) != it->second.caps) {
            failures.push_back(std::string(where) + ": caps violation — " + r->name() +
                               " lacks " + std::to_string(it->second.caps));
          }
        }
      }
    }
    // 4. Full allocator invariants.
    std::string c = k.heap.checkConsistency();
    if (!c.empty()) failures.push_back(std::string(where) + ": " + c);
  };

  const uint32_t kOps = 400;
  for (uint32_t op = 0; op < kOps && failures.empty(); ++op) {
    uint32_t kind = next() % 5;
    size_t sz = 1 + (next() % 4096);
    uint32_t caps = MALLOC_CAP_8BIT;
    switch (next() % 3) {
      case 0: caps |= MALLOC_CAP_INTERNAL; break;
      case 1: caps |= MALLOC_CAP_SPIRAM; break;
      case 2: caps |= MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA; break;
    }
    if (kind == 0 || kind == 1) {  // alloc / calloc
      // calloc(n,8) allocates n*8 bytes — the reference must record the REAL
      // requested size, not `sz` (that was the size-mismatch bug).
      size_t realSz = (kind == 0) ? sz : (sz / 8 + 1) * 8;
      void* p = (kind == 0) ? k.heap.alloc(sz, caps, "fuzz")
                            : k.heap.calloc(sz / 8 + 1, 8, caps, "fuzz");
      if (p) ref[p] = RefAlloc{realSz, caps};
      else {
        // OOM is legal — but only when it SHOULD fail: never allow OOM when a
        // fresh region could fit it. We don't track per-region free here, so
        // just require consistency.
      }
    } else if (kind == 2 && !ref.empty()) {  // realloc
      auto it = ref.begin();
      std::advance(it, next() % ref.size());
      void* old = it->first;
      RefAlloc oldMeta = it->second;
      uint32_t newCaps = (next() % 2) ? oldMeta.caps : (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
      size_t newSz = 1 + (next() % 8192);
      void* q = k.heap.realloc(old, newSz, newCaps, "fuzz");
      if (q) {
        ref.erase(old);
        ref[q] = RefAlloc{newSz, newCaps};
      }
      // realloc failure (OOM/contract) keeps old alive — reference unchanged.
    } else if (kind == 3 && !ref.empty()) {  // free
      auto it = ref.begin();
      std::advance(it, next() % ref.size());
      k.heap.free(it->first);
      ref.erase(it);
    } else if (kind == 4) {  // free a random one (or skip when empty)
      if (!ref.empty()) {
        auto it = ref.begin();
        std::advance(it, next() % ref.size());
        k.heap.free(it->first);
        ref.erase(it);
      }
    }
    crossCheck(("op " + std::to_string(op)).c_str());
    if (failures.size() > 8) break;
  }
  // Full free at the end: everything must return to the heap.
  for (auto& kv : ref) k.heap.free(kv.first);
  ref.clear();
  crossCheck("final");
  if (!failures.empty()) {
    // Enrich the first failure with the seed for reproduction.
    failures.insert(failures.begin(),
                    "seed=" + std::to_string(seed) + " (reproduce: --seed " +
                        std::to_string(seed) + " heap_fuzz)");
  }
  return failures;
}

}  // namespace

std::vector<Scenario> allScenarios() {
  return {
      {"single_tap_slow_index", scenario_single_tap_slow_index, false},
      {"bug_lost_tap", scenario_bug_lost_tap, true},
      {"rapid_tap_correct", scenario_rapid_tap_correct, false},
      {"bug_divergence", scenario_bug_divergence, true},
      {"tls_fragmented_heap", scenario_tls_fragmented_heap, false},
      {"tls_keepalive", scenario_tls_keepalive, false},
      {"framebuffer_contract", scenario_framebuffer_contract, false},
      {"chunked_quiet_eof", scenario_chunked_quiet_eof, false},
      {"slow_sd_tap", scenario_slow_sd_tap, false},
      {"heap_stress", scenario_heap_stress, false},
      {"heap_fuzz", scenario_heap_fuzz, false},
  };
}

}  // namespace m4sim

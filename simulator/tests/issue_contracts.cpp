#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "core/SimKernel.h"
#include "memory/SimHeap.h"
#include "model/SimChapterLifecycle.h"
#include "storage/SimAtomicFileCommit.h"

using namespace m4sim;

namespace {
int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

void issue9_tls_control_plane_must_stay_internal() {
  SimTrace trace;
  SimHeap heap(&trace);

  heap.expect(MemoryContract{"native_tls_worker_stack",
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 64 * 1024});
  heap.expect(MemoryContract{"wifi_client_secure_control",
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 16 * 1024});
  heap.expect(MemoryContract{"http_client_control",
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 16 * 1024});
  heap.expect(MemoryContract{"https_body_payload",
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 512 * 1024});
  heap.expect(MemoryContract{"chapter_decode_payload",
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 1024 * 1024});

  check(heap.alloc(48 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                   "native_tls_worker_stack") == nullptr,
        "issue #9: PSRAM TLS worker stack placement must violate residency contract");
  check(heap.alloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                   "wifi_client_secure_control") == nullptr,
        "issue #9: WiFiClientSecure control object in PSRAM must be rejected");
  check(heap.alloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                   "http_client_control") == nullptr,
        "issue #9: HTTPClient control object in PSRAM must be rejected");
  check(heap.alloc(64 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                   "https_body_payload") == nullptr,
        "issue #9: large HTTPS body in internal RAM must be rejected");

  void* stack = heap.alloc(48 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                           "native_tls_worker_stack");
  void* secure = heap.alloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                            "wifi_client_secure_control");
  void* http = heap.alloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                          "http_client_control");
  void* body = heap.alloc(128 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                          "https_body_payload");
  void* decode = heap.alloc(256 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                            "chapter_decode_payload");
  check(stack && secure && http && body && decode,
        "issue #9: internal control plane + PSRAM payload policy must fit fresh S3 heap");
  check(heap.contractViolations() == 4,
        "issue #9: every deliberately wrong residency must produce a contract violation");

  heap.free(stack);
  heap.free(secure);
  heap.free(http);
  heap.free(body);
  heap.free(decode);
  check(heap.liveAllocs().empty(), "issue #9: request lifecycle must release every modeled allocation");
}

void issue5_chapter_switch_never_opens_empty_path_or_leaks_generations() {
  SimChapterLifecycle lifecycle(/*watchdogBudgetMs=*/5000);
  check(lifecycle.openInitial(0, 1, "cache/book/ch_1.txt"),
        "issue #5: initial chapter opens");
  uint32_t now = 100;
  for (int chapter = 2; chapter <= 80; ++chapter) {
    check(lifecycle.requestSwitch(now, chapter), "issue #5: cross-chapter intent accepted");
    now += 20;
    check(lifecycle.readerClosed(now), "issue #5: old reader generation closes before download/open");
    now += 80;
    check(lifecycle.contentReady(now, chapter,
                                 "cache/book/ch_" + std::to_string(chapter) + ".txt", true),
          "issue #5: only committed non-empty chapter path becomes ready");
    now += 10;
    check(lifecycle.openReady(now), "issue #5: next reader generation opens");
    check(lifecycle.checkWatchdog(now), "issue #5: transition stays inside watchdog budget");
    now += 50;
  }
  check(lifecycle.currentChapter() == 80, "issue #5: repeated handoff lands on final chapter");
  check(lifecycle.generation() == 80, "issue #5: exactly one reader generation per chapter");
  check(lifecycle.closedGenerations() == 79,
        "issue #5: every replaced generation was explicitly released");
  check(!lifecycle.currentPath().empty(), "issue #5: authoritative reader path is never empty");
  check(lifecycle.ok(), "issue #5: long transition chain remains internally consistent");

  SimChapterLifecycle emptyPath;
  check(emptyPath.openInitial(0, 1, "cache/book/ch_1.txt"), "negative setup opens chapter 1");
  check(emptyPath.requestSwitch(10, 2), "negative setup stores chapter 2 intent");
  check(emptyPath.readerClosed(20), "negative setup releases chapter 1");
  check(!emptyPath.contentReady(30, 2, "", true),
        "issue #5: historical empty-relPath handoff is rejected before reader open");

  SimChapterLifecycle partialFile;
  check(partialFile.openInitial(0, 1, "cache/book/ch_1.txt"), "partial setup opens chapter 1");
  check(partialFile.requestSwitch(10, 2), "partial setup stores intent");
  check(partialFile.readerClosed(20), "partial setup closes reader");
  check(!partialFile.contentReady(30, 2, "cache/book/ch_2.txt", false),
        "issue #5/#24: partially committed chapter must not enter TxtReader");
}

std::vector<uint8_t> bytes(size_t n, uint8_t seed) {
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(seed + i * 31u);
  return out;
}

void issue24_atomic_sd_commit_preserves_previous_generation() {
  SimAtomicFileStore fs;
  const auto old = bytes(4096, 3);
  const auto next = bytes(12137, 9);
  fs.put("catalog.json", old);
  fs.put("catalog.tmp", next);

  SimAtomicFileStore::Faults renameFallback;
  renameFallback.renameFails = true;
  fs.setFaults(renameFallback);
  auto ok = fs.commit("catalog.tmp", "catalog.json");
  check(ok.ok && ok.usedCopyFallback && !ok.usedRename,
        "issue #24: FAT/exFAT rename failure uses streaming-copy fallback");
  check(ok.copiedMaxChunk <= 2048,
        "issue #24: copy fallback never requires an unbounded internal-RAM buffer");
  check(fs.get("catalog.json") && *fs.get("catalog.json") == next,
        "issue #24: verified fallback publishes complete new generation");

  SimAtomicFileStore writeFail;
  writeFail.put("catalog.json", old);
  writeFail.put("catalog.tmp", next);
  SimAtomicFileStore::Faults f;
  f.renameFails = true;
  f.copyWriteFails = true;
  writeFail.setFaults(f);
  auto failed = writeFail.commit("catalog.tmp", "catalog.json");
  check(!failed.ok, "issue #24: injected SD write failure is surfaced");
  check(writeFail.get("catalog.json") && *writeFail.get("catalog.json") == old,
        "issue #24: failed replacement preserves previous authoritative catalog");

  SimAtomicFileStore truncate;
  truncate.put("chapter.txt", old);
  truncate.put("chapter.tmp", next);
  f = {};
  f.renameFails = true;
  f.truncateFinal = true;
  truncate.setFaults(f);
  auto badSize = truncate.commit("chapter.tmp", "chapter.txt");
  check(!badSize.ok, "issue #24: final byte-size mismatch rejects generation");
  check(truncate.get("chapter.txt") && *truncate.get("chapter.txt") == old,
        "issue #24: size-verification failure never destroys the prior chapter");
}

}  // namespace

int main() {
  issue9_tls_control_plane_must_stay_internal();
  issue5_chapter_switch_never_opens_empty_path_or_leaks_generations();
  issue24_atomic_sd_commit_preserves_previous_generation();
  if (failures) {
    std::cerr << failures << " issue-contract test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "PASS firmware issue mechanism contracts\n";
  return EXIT_SUCCESS;
}

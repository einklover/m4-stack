// Host regression for the Fanqie 正文 reader-handoff boundary (no SD/network).
// Exercises the real M4ContentProviderSession registry — the exact state
// NativeProviderBookActivity::openReadyReader and AppRuntimeActivity::
// tryLaunchPluginReader consume — so a future refactor cannot silently let
// bad metadata or an unsafe cache path reach TxtReaderActivity.
//
// Contract groups:
//   1. safe handoff: register → Fetching → Ready(safe .txt rel path) is
//      observable via chapterAt, the precondition both launch sites poll.
//   2. bad content/metadata: traversal or absolute cacheRelPath never becomes
//      Ready; Error clears any stale path; oversized/invalid ids rejected.
//   3. bounded prefetch: duplicate work items coalesce; Ready drains the queue.
//   4. clearForApp wipes all session state (no cross-app leakage).

#include "apps/M4ContentProviderSession.h"
#include "util/M4ContentProviderContract.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace M4ContentProvider;
namespace S = M4ContentProviderSession;

namespace {

BookSpec fanqieSpec(const std::string& bookId) {
  BookSpec spec;
  spec.providerId = "fanqie";
  spec.bookId = bookId;
  spec.title = "测试书籍";
  spec.appId = "com.fanqie.client";
  spec.catalog.kind = ChapterCatalogKind::FileRows;
  spec.catalog.chapterCount = 3;
  spec.catalog.fileRelPath = "toc_rows.txt";
  spec.catalog.uidField0 = 0;
  spec.catalog.titleField0 = 1;
  spec.cachePolicy.maxChapterBytes = 4u * 1024u * 1024u;
  return spec;
}

ChapterStatus readyStatus(const std::string& bookId, int idx, const std::string& uid,
                          const std::string& relPath) {
  ChapterStatus st;
  st.providerId = "fanqie";
  st.bookId = bookId;
  st.chapterUid = uid;
  st.index0 = idx;
  st.state = ChapterReady::Ready;
  st.cacheRelPath = relPath;
  return st;
}

void safeHandoff() {
  const std::string book = "handoff1";
  assert(S::registerBook(fanqieSpec(book)));

  // Before fetch: Missing with no path — openReadyReader must refuse.
  const auto missing = S::chapterAt("fanqie", book, 0);
  assert(missing.state == ChapterReady::Missing);
  assert(missing.cacheRelPath.empty());

  // Fetching without a resolved uid (FileRows) is invalid.
  ChapterStatus noUid;
  noUid.providerId = "fanqie";
  noUid.bookId = book;
  noUid.index0 = 0;
  noUid.state = ChapterReady::Fetching;
  assert(!S::setChapterStatus(noUid));

  // Fetching with uid ok.
  ChapterStatus fetching = noUid;
  fetching.chapterUid = "7123456789000000001";
  assert(S::setChapterStatus(fetching));

  // Ready with a safe app-data-relative .txt path: exactly what the reader
  // launch sites need.
  assert(S::setChapterStatus(
      readyStatus(book, 0, "7123456789000000001", "chapters/7123456789/ch_7123456789000000001.txt")));
  const auto ready = S::chapterAt("fanqie", book, 0);
  assert(ready.state == ChapterReady::Ready);
  assert(ready.pct == 100);
  assert(ready.error.empty());
  assert(ready.cacheRelPath == "chapters/7123456789/ch_7123456789000000001.txt");
  assert(ready.chapterUid == "7123456789000000001");
}

void badContentAndMetadata() {
  const std::string book = "handoff2";
  assert(S::registerBook(fanqieSpec(book)));
  const std::string uid = "7123456789000000002";

  // A Fetching chapter must stay Fetching when a malformed Ready is rejected:
  // setChapterStatus is atomic — no phantom Ready with an empty path.
  ChapterStatus fetching;
  fetching.providerId = "fanqie";
  fetching.bookId = book;
  fetching.chapterUid = uid;
  fetching.index0 = 1;
  fetching.state = ChapterReady::Fetching;
  fetching.pct = 40;
  assert(S::setChapterStatus(fetching));

  // Path traversal must never become a Ready reader source.
  const char* unsafePaths[] = {
      "../escape.txt",
      "chapters/../../escape.txt",
      "/absolute/path.txt",
      "\\windows\\path.txt",
      "chapters/dir..double/txt",   // ".." anywhere
      "short",                      // not .txt
      "",                           // empty
  };
  for (const auto* rel : unsafePaths) {
    ChapterStatus st = readyStatus(book, 1, uid, rel);
    const bool accepted = S::setChapterStatus(st);
    assert(!accepted);
    const auto at = S::chapterAt("fanqie", book, 1);
    // Atomicity: rejection leaves the pre-call Fetching status intact.
    assert(at.state == ChapterReady::Fetching);
    assert(at.pct == 40);
    assert(at.cacheRelPath.empty());
  }

  // Error status clears any previously cached path so a stale/partial body
  // cannot be reopened by resume/history.
  const std::string book3 = "handoff3";
  assert(S::registerBook(fanqieSpec(book3)));
  const std::string uid3 = "7123456789000000003";
  assert(S::setChapterStatus(readyStatus(book3, 0, uid3, "chapters/b3/ch_a.txt")));
  ChapterStatus err;
  err.providerId = "fanqie";
  err.bookId = book3;
  err.chapterUid = uid3;
  err.index0 = 0;
  err.state = ChapterReady::Error;
  err.error = "http_2xx_empty";
  assert(S::setChapterStatus(err));
  const auto afterErr = S::chapterAt("fanqie", book3, 0);
  assert(afterErr.state == ChapterReady::Error);
  assert(afterErr.cacheRelPath.empty());

  // Invalid identities are rejected outright.
  ChapterStatus badBook = readyStatus(book3, 1, uid3, "chapters/x/ch_b.txt");
  badBook.bookId = "";  // idOk fails on empty
  assert(!S::setChapterStatus(badBook));
  ChapterStatus badProvider = readyStatus(book3, 1, uid3, "chapters/x/ch_c.txt");
  badProvider.providerId = "fan qie";  // space rejected by idOk
  assert(!S::setChapterStatus(badProvider));

  // Registry-level spec validation: bad provider id / catalog shape.
  BookSpec bad = fanqieSpec("specbad");
  bad.providerId = "";
  assert(!S::registerBook(bad));
  BookSpec badCatalog = fanqieSpec("specbad2");
  badCatalog.catalog.fileRelPath = "../toc.txt";
  assert(!S::registerBook(badCatalog));
  BookSpec zeroCount = fanqieSpec("specbad3");
  zeroCount.catalog.chapterCount = 0;
  assert(!S::registerBook(zeroCount));
}

void boundedPrefetchQueue() {
  const std::string book = "prefetch1";
  assert(S::registerBook(fanqieSpec(book)));

  // Duplicate prefetch requests coalesce to one work item.
  assert(S::requestPrefetch("fanqie", book, 0));
  assert(S::requestPrefetch("fanqie", book, 0));
  size_t queued = 0;
  while (true) {
    const auto w = S::pollWork();
    if (!w.valid) break;
    ++queued;
  }
  assert(queued == 1);

  // Out-of-range index rejected.
  assert(!S::requestPrefetch("fanqie", book, 99));
  assert(!S::requestPrefetch("weread", book, 0));  // not registered

  // Ready drains matching work; queue stays bounded across many requests.
  for (int round = 0; round < 8; ++round) {
    for (int i = 0; i < 3; ++i) S::requestPrefetch("fanqie", book, i);
  }
  assert(S::setChapterStatus(
      readyStatus(book, 0, "7123456789000000010", "chapters/pf/ch_p0.txt")));
  size_t remainingIdx0 = 0;
  while (true) {
    const auto w = S::pollWork();
    if (!w.valid) break;
    if (w.index0 == 0) ++remainingIdx0;
  }
  assert(remainingIdx0 == 0);  // every queued idx0 item was drained by Ready

  S::clearForApp("com.fanqie.client");
  const auto gone = S::chapterAt("fanqie", book, 0);
  assert(gone.state == ChapterReady::Missing);
}

}  // namespace

int main() {
  safeHandoff();
  badContentAndMetadata();
  boundedPrefetchQueue();
  std::cout << "fanqie reader-handoff contract tests passed\n";
  return 0;
}

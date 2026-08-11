#pragma once
// Production open-path terminalization for ReaderActivity (host-testable).
// Every book open must leave M4IndexingState::Session in Ready/Failed/Cancelled.

#include <cstdint>

#include "M4IndexingState.h"

namespace M4ReaderOpenDispatch {

// Call when an open path completes successfully (direct TXT, XTC, EPUB, convert).
// Clears Preparing/Indexing busy; if Session was Idle (cache hit), still records Ready.
inline void onOpenSuccess(M4IndexingState::Machine& m, const char* msg = "ready") {
  using namespace M4IndexingState;
  if (m.snap().busy || m.snap().phase == Phase::Preparing || m.snap().phase == Phase::Indexing) {
    m.succeed(0, msg ? msg : "ready");
  } else {
    m.reset();
    m.beginPreparing(msg ? msg : "ready");
    m.succeed(0, msg ? msg : "ready");
  }
}

// Call before onGoBack when load/open fails (never leave busy).
inline void onOpenFail(M4IndexingState::Machine& m, const char* msg = "failed") {
  m.fail(0, msg ? msg : "failed");
}

// onEnter: cache dir missing → Preparing (UI loading).
inline void onOpenBeginPreparing(M4IndexingState::Machine& m, const char* msg = "open_book") {
  m.reset();
  m.beginPreparing(msg ? msg : "open_book");
}

// Dispatch scenarios for tests / documentation of ReaderActivity paths.
enum class Kind : uint8_t { Xtc, TxtDirect, TxtConvert, Epub };

// Simulate the terminal step after a load attempt (success or fail).
inline void applyDispatchTerminal(M4IndexingState::Machine& m, Kind kind, bool loadOk) {
  if (!loadOk) {
    switch (kind) {
      case Kind::Xtc: onOpenFail(m, "xtc_load"); break;
      case Kind::TxtDirect:
      case Kind::TxtConvert: onOpenFail(m, "txt_load"); break;
      case Kind::Epub: onOpenFail(m, "epub_load"); break;
    }
    return;
  }
  switch (kind) {
    case Kind::Xtc: onOpenSuccess(m, "xtc_open"); break;
    case Kind::TxtDirect: onOpenSuccess(m, "direct_txt"); break;
    case Kind::TxtConvert: onOpenSuccess(m, "txt_via_epub"); break;
    case Kind::Epub: onOpenSuccess(m, "epub_open"); break;
  }
}

}  // namespace M4ReaderOpenDispatch

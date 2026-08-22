// ReaderModel: a thin adapter wiring the SHARED PageTurnCoordinator (the same
// production state machine the real TxtReaderActivity runs) to platform ports.
// All page-turn decisions — quickMode, catch-up, pending snapshot,
// physical-from-provenance — live in core/PageTurnCoordinator.h, so the code
// under test is identical to firmware, not a human mirror.
#pragma once

#include <cstdint>
#include <functional>

#include "core/PageTurnCoordinator.h"
#include "core/SimKernel.h"
#include "platform/DisplayPort.h"
#include "platform/StoragePort.h"

namespace m4sim {

class ReaderModel {
public:
  struct Knobs {
    bool bugNoCatchup = false;    // remove catch-up invariant → lost intent
    bool bugLivePhysical = false; // physical = live currentPage → divergence
    double sdScale = 1.0;         // SD latency multiplier (slow SD)
    uint32_t indexSlicePages = 16;  // progressive index pages per SD slice
    bool animEnabled = true;      // page-turn wipe (longer EPD busy)
  };

  ReaderModel(SimScheduler* sched, SimTrace* trace,
              m4platform::DisplayPort* panel, m4platform::StoragePort* sd,
              Knobs knobs)
      : sched_(sched), trace_(trace), panel_(panel), sd_(sd), knobs_(knobs),
        coord_(m4reader::PageTurnCoordinator::TestPolicy{
            knobs.bugNoCatchup, knobs.bugLivePhysical, 400}) {}

  // Open a book with `totalPages` pages; index starts incomplete.
  void openBook(int totalPages) {
    totalPages_ = totalPages;
    indexCursor_ = 0;
    indexComplete_ = (totalPages == 0);
    generation_ = 0;
    tapCount_ = 0;
    coord_.reset();
    if (totalPages_ > 0) indexComplete_ = false;
    sched_->every(kDisplayTickMs, [this]() { displayTaskLoop(); });
  }

  // Input path: a page-turn intent. All state-machine decisions delegate to
  // the shared coordinator.
  void tap(int dir) {
    tapCount_++;
    uint32_t now = sched_->now();
    int newTarget = clamp(coord_.targetPage() + dir);
    coord_.onTap(now, panel_->busy(), newTarget);
    char buf[64];
    snprintf(buf, sizeof(buf), "dir=%d quick=%d target=%d/%d busy=%d", dir,
             coord_.quickMode() ? 1 : 0, newTarget, totalPages_, panel_->busy() ? 1 : 0);
    trace_->emit(SimEventType::INPUT, buf, now);
    trace_->emit(SimEventType::PAGE_TARGET, "target=" + std::to_string(newTarget), now);
  }

  // Strict single-intent test helper: jump the TARGET straight to an absolute
  // page (as if the user pressed NEXT repeatedly fast enough to skip pages)
  // WITHOUT waiting for the index. Returns true if the intent was accepted
  // (target beyond current index).
  bool jumpTo(int page) {
    tapCount_++;
    page = clamp(page);
    coord_.onTap(sched_->now(), panel_->busy(), page);
    trace_->emit(SimEventType::INPUT, "jump target=" + std::to_string(page), sched_->now());
    trace_->emit(SimEventType::PAGE_TARGET, "target=" + std::to_string(page), sched_->now());
    return (page >= indexCursor_);
  }

  // ── state (probes delegate to coordinator) ──────────────────────────────
  int targetPage() const { return coord_.targetPage(); }
  int physicalPage() const { return coord_.physicalPage(); }
  int firmwarePhysicalPage() const { return coord_.physicalPage(); }
  bool panelIdle() const { return !panel_->busy(); }
  bool firstShown() const { return coord_.firstShown(); }
  bool indexComplete() const { return indexComplete_; }
  int totalPages() const { return totalPages_; }
  int generation() const { return generation_; }
  int indexCursor() const { return indexCursor_; }
  int tapCount() const { return tapCount_; }
  bool renderInFlight() const { return renderInFlight_; }
  bool epdBusy() const { return panel_->busy(); }

private:
  static constexpr uint32_t kDisplayTickMs = 20;

  int clamp(int p) const {
    if (p < 0) return 0;
    if (p >= totalPages_) return totalPages_ > 0 ? totalPages_ - 1 : 0;
    return p;
  }

  // Continue the progressive page index by one storage slice (async).
  void continuePageIndex() {
    if (indexComplete_) return;
    trace_->emit(SimEventType::INDEX_STARTED,
                 "cursor=" + std::to_string(indexCursor_) + "/" + std::to_string(totalPages_),
                 sched_->now());
    sd_->read("index", 4096, knobs_.sdScale, [this](size_t n) {
      if (n == 0) return;  // short read: retry next tick
      indexCursor_ += knobs_.indexSlicePages;
      if (indexCursor_ >= totalPages_) {
        indexCursor_ = totalPages_;
        indexComplete_ = true;
        trace_->emit(SimEventType::INDEX_READY, "cursor=" + std::to_string(indexCursor_),
                     sched_->now());
      }
    });
  }

  void displayTaskLoop() {
    bool busy = panel_->busy();
    if (busy) return;  // panel owned by in-flight refresh

    // ── catch-up invariant (delegated) ─────────────────────────────────
    if (coord_.catchupNeeded(busy, false)) {
      if (coord_.targetBeyondIndex(indexComplete_, indexCursor_)) {
        // Target not indexed yet: push the progressive index until it covers
        // the target; next idle tick renders straight to it (one burst).
        continuePageIndex();
        return;
      }
      coord_.forceCatchupArm();
    }
    if (knobs_.bugNoCatchup && !coord_.updateRequired()) return;

    if (!coord_.shouldRender(busy, false)) return;

    // A render is already in flight (storage read pending before EPD submit) —
    // do not stack another layout of the same target while the first runs.
    if (renderInFlight_) return;

    // Target not indexed yet → keep the refresh armed and push the index.
    if (coord_.targetBeyondIndex(indexComplete_, indexCursor_)) {
      continuePageIndex();
      return;
    }

    // ── render: snapshot the page the EPD will show (delegated) ────────
    coord_.onRenderStarted();
    int renderedPage = coord_.onFrameReady();
    size_t pageBytes = 48000;  // 800x480 1bpp framebuffer
    uint32_t gen = ++generation_;
    renderInFlight_ = true;
    sd_->read("page", pageBytes, knobs_.sdScale, [this, renderedPage, gen](size_t n) {
      renderInFlight_ = false;
      if (n == 0) {
        // Short read fault: retry next tick, keep the intent armed.
        coord_.rearm();
        return;
      }
      uint64_t hash = (uint64_t)renderedPage * 2654435761u + gen;
      m4platform::FrameTag tag;
      tag.generation = gen;
      tag.chapter = 0;
      tag.page = renderedPage;
      tag.frameHash = hash;
      panel_->render(tag);

      // ── submit to display backend (async BUSY/commit) ────────────────
      // Navigation/page animation never requests the legacy full waveform;
      // the production reader selects its explicit cleanup mode only after
      // the configured page-turn cadence.
      m4platform::RefreshMode mode = m4platform::RefreshMode::FAST_REFRESH;
      bool accepted = panel_->submit(mode, [this, renderedPage]() {
        // Commit callback means the backend now physically presents this frame.
        bool catchup = coord_.onCommitted(sched_->now(), renderedPage);
        trace_->emit(SimEventType::STATE_CHANGED,
                     "lastPhysicalBodyPage_=" + std::to_string(coord_.physicalPage()) +
                         " (committed frame provenance)",
                     sched_->now());
        if (catchup) {
          trace_->emit(SimEventType::PAGE_TARGET,
                       "anim_catchup_pending target=" + std::to_string(coord_.targetPage()) +
                           " body=" + std::to_string(coord_.physicalPage()),
                       sched_->now());
        }
      });
      if (!accepted) {
        // Backend busy when we tried (shouldn't happen — we gate on busy). Retry.
        coord_.rearm();
      }
    });
  }

  SimScheduler* sched_;
  SimTrace* trace_;
  m4platform::DisplayPort* panel_;
  m4platform::StoragePort* sd_;
  Knobs knobs_;
  m4reader::PageTurnCoordinator coord_;

  bool indexComplete_ = true;
  int totalPages_ = 0;
  int indexCursor_ = 0;
  int generation_ = 0;
  int tapCount_ = 0;
  bool renderInFlight_ = false;
};

}  // namespace m4sim

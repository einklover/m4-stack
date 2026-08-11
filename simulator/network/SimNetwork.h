// SimNetwork: raw HTTP chunked-stream fixture + TLS internal-RAM cost model.
//
// The chunked fixture feeds the firmware's TCP stream byte-by-byte over virtual
// time, so the old "quiet-EOF = 700ms" truncation bug reproduces exactly (curl
// would have hidden it). TLS handshake "reserves" a block of contiguous
// INTERNAL RAM the way mbedtls does — a fragmented internal heap makes it OOM
// even when PSRAM is nearly free.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "core/SimKernel.h"
#include "memory/SimHeap.h"

namespace m4sim {

// A chunked HTTP/1.1 response body served as raw bytes over virtual time.
struct RawChunk {
  std::string bytes;       // already includes size line + \r\n terminators
  uint32_t arrivesAtMs;    // virtual time the chunk becomes readable
};

// Real streaming chunked-transfer decoder. `feed()` accepts ARBITRARY TCP
// fragments — a size line may arrive in pieces, data may be split across
// reads, CR/LF may straddle buffer boundaries. State machine:
//   WANT_SIZE → WANT_DATA → WANT_CR → WANT_LF → WANT_SIZE …
// A 0-size chunk (with its trailer) is the true EOF. `quietEofMs` models the
// buggy "silence means EOF" heuristic that truncated the 466c-byte chapter on
// real devices; set to 0 to require the real 0-chunk.
class ChunkedDecoder {
public:
  enum class State { WANT_SIZE, WANT_DATA, WANT_CR, WANT_LF, WANT_TRAILER_LF, DONE, TRUNCATED };

  ChunkedDecoder(uint32_t quietEofMs = 0) : quietEofMs_(quietEofMs) {}

  // Feed an arbitrary TCP fragment (may split size line / data / CRLF any way).
  State feed(const char* data, size_t len, uint32_t nowMs) {
    if (state_ == State::DONE || state_ == State::TRUNCATED) return state_;
    pending_.append(data, len);
    while (true) {
      switch (state_) {
        case State::WANT_SIZE: {
          size_t crlf = pending_.find("\r\n");
          if (crlf == std::string::npos) {
            // Wait for the rest of the size line (bounded to avoid a lie).
            if (pending_.size() > kMaxSizeLine) { state_ = State::TRUNCATED; return state_; }
            return state_;
          }
          std::string hexSize = pending_.substr(0, crlf);
          size_t size = (size_t)strtoul(hexSize.c_str(), nullptr, 16);
          pending_.erase(0, crlf + 2);
          if (size == 0) {
            // 0-chunk: the terminator is "0\r\n" then an empty trailer "\r\n".
            state_ = State::WANT_TRAILER_LF;
            lastActivityMs_ = nowMs;
            break;
          }
          dataRemaining_ = size;
          chunksDelivered_++;
          state_ = State::WANT_DATA;
          break;
        }
        case State::WANT_DATA: {
          size_t take = std::min(dataRemaining_, pending_.size());
          body_.append(pending_, 0, take);
          pending_.erase(0, take);
          dataRemaining_ -= take;
          if (dataRemaining_ == 0) state_ = State::WANT_CR;
          else return state_;
          break;
        }
        case State::WANT_CR: {
          if (pending_.empty()) return state_;
          if (pending_[0] != '\r') { state_ = State::TRUNCATED; return state_; }
          pending_.erase(0, 1);
          state_ = State::WANT_LF;
          break;
        }
        case State::WANT_LF: {
          if (pending_.empty()) return state_;
          if (pending_[0] != '\n') { state_ = State::TRUNCATED; return state_; }
          pending_.erase(0, 1);
          lastActivityMs_ = nowMs;
          state_ = State::WANT_SIZE;  // next chunk (or 0-chunk = DONE)
          break;
        }
        case State::WANT_TRAILER_LF: {
          // After the 0-chunk's size line, expect the trailer "\r\n".
          if (pending_.size() < 2) return state_;
          if (pending_[0] != '\r' || pending_[1] != '\n') {
            state_ = State::TRUNCATED;
            return state_;
          }
          pending_.erase(0, 2);
          lastActivityMs_ = nowMs;
          state_ = State::DONE;
          return state_;
        }
        default:
          return state_;
      }
    }
  }

  State feed(const std::string& data, uint32_t nowMs) {
    return feed(data.data(), data.size(), nowMs);
  }

  // Poll for the buggy quiet-EOF heuristic. If no data arrived within
  // quietEofMs_ AND quietEofMs_>0, declare TRUNCATED (the historical bug).
  State poll(uint32_t nowMs) {
    if (quietEofMs_ > 0 && state_ != State::DONE && state_ != State::TRUNCATED) {
      if (nowMs - lastActivityMs_ >= quietEofMs_) state_ = State::TRUNCATED;
    }
    return state_;
  }

  State state() const { return state_; }
  const std::string& body() const { return body_; }
  int chunksDelivered() const { return chunksDelivered_; }

private:
  // WANT_LF_AFTER_ZERO_ is internal: after a "0\r\n" the RFC requires "\r\n".
  // We reuse WANT_LF with a flag to avoid extra public states.
  static constexpr size_t kMaxSizeLine = 1024;

  uint32_t quietEofMs_;
  uint32_t lastActivityMs_ = 0;
  State state_ = State::WANT_SIZE;
  std::string pending_;
  std::string body_;
  size_t dataRemaining_ = 0;
  int chunksDelivered_ = 0;
};

// TLS session cost model. Two distinct INTERNAL-RAM costs (the review's point:
// holding the full handshake buffer for the connection's lifetime overstates
// keep-alive pressure):
//   peakBytes    — transient mbedtls handshake buffers; freed after handshake.
//   residentBytes— what the connection keeps for its lifetime (session cache,
//                  partial buffers). The 48KB reserve from the TLS OOM story is
//                  the PEAK; keep-alive reuse pays only the resident cost.
// A new connection must fit the PEAK in contiguous INTERNAL RAM. Reusing a
// keep-alive connection pays nothing new.
class SimTls {
public:
  SimTls(SimHeap* heap, SimTrace* trace, SimScheduler* sched = nullptr)
      : heap_(heap), trace_(trace), sched_(sched),
        peakBytes_(48 * 1024), residentBytes_(12 * 1024) {}

  // Try to open a fresh TLS session. Returns false if the internal heap cannot
  // provide one contiguous handshake-peak block (the historical TLS OOM).
  bool open(const char* tag) {
    uint32_t t = sched_ ? sched_->now() : 0;
    trace_->emit(SimEventType::TLS_BEGIN,
                 "handshake peak=" + std::to_string(peakBytes_) + "B tag=" + tag, t);
    if (resident_ != nullptr) {
      // keep-alive reuse: no new handshake, no new peak
      trace_->emit(SimEventType::TLS_BEGIN, "reuse keep-alive (no reserve)", t);
      return true;
    }
    void* peak = heap_->alloc(peakBytes_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, tag);
    if (!peak) {
      // OOM already recorded by the heap; surface it.
      return false;
    }
    // Handshake completes: release the transient peak, hold the resident cost.
    heap_->free(peak);
    resident_ = heap_->alloc(residentBytes_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, tag);
    if (!resident_) {
      // Resident alloc failed (e.g. heap churn between peak free and resident
      // alloc). open() must NOT claim success without a held connection — the
      // OOM is already recorded by the heap.
      trace_->emit(SimEventType::TLS_END,
                   "resident alloc failed — connection not established", t);
      return false;
    }
    trace_->emit(SimEventType::TLS_END,
                 "handshake done resident=" + std::to_string(residentBytes_) +
                     "B internal_free=" + std::to_string(heap_->freeInternal()),
                 t);
    return true;
  }

  void close() {
    uint32_t t = sched_ ? sched_->now() : 0;
    if (resident_) {
      heap_->free(resident_);
      resident_ = nullptr;
      trace_->emit(SimEventType::TLS_END, "closed", t);
    }
  }

  bool isOpen() const { return resident_ != nullptr; }
  size_t peakBytes() const { return peakBytes_; }
  size_t residentBytes() const { return residentBytes_; }
  void setPeakBytes(size_t b) { peakBytes_ = b; }
  void setResidentBytes(size_t b) { residentBytes_ = b; }

private:
  SimHeap* heap_;
  SimTrace* trace_;
  SimScheduler* sched_;
  size_t peakBytes_;
  size_t residentBytes_;
  void* resident_ = nullptr;
};

// HTTP request cost model (uncompressed connect/header, cheap on the heap —
// the real pressure is TLS + response buffers).
class SimHttp {
public:
  explicit SimHttp(SimScheduler* sched) : sched_(sched) {}

  void get(const char* /*url*/, std::function<void(int statusCode)> onDone) {
    sched_->scheduleIn(200, [onDone]() { onDone(200); });
  }

private:
  SimScheduler* sched_;
};

}  // namespace m4sim

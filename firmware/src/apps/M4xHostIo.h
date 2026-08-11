#pragma once

// Host-testable policy and transaction primitives shared by M4x download APIs.
// This file deliberately has no Arduino/SD dependency.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace M4xHostIo {

enum class Operation : uint8_t {
  JsonGet,
  JsonToFile,
  Download,
};

struct Permissions {
  bool network = false;
  bool appData = false;
};

inline const char* permissionError(const Permissions& p, Operation op) {
  if (!p.network) return "permission denied: network";
  if ((op == Operation::JsonToFile || op == Operation::Download) && !p.appData) {
    return "permission denied: filesystem.appdata";
  }
  return nullptr;
}

struct Limits {
  // jsonGet buffers the whole body (PSRAM when present) before projecting
  // rows into Lua.  The 192KiB hard cap rejected large shelf/catalog
  // responses (WeRead shelf with 200+ books → "response_too_large"); the Lua
  // heap headroom check after projection is the real guard, so allow large
  // bodies up to the stream cap.  Default stays small for unstated requests.
  static constexpr size_t kJsonGetDefault = 160 * 1024;
  static constexpr size_t kJsonGetHard = 4 * 1024 * 1024;
  static constexpr size_t kStreamDefault = 768 * 1024;
  static constexpr size_t kStreamHard = 4 * 1024 * 1024;
  static constexpr uint32_t kTimeoutDefaultMs = 30000;
  static constexpr uint32_t kTimeoutMinMs = 1000;
  static constexpr uint32_t kTimeoutMaxMs = 60000;
  static constexpr size_t kMaxHeaders = 24;
  static constexpr size_t kMaxHeaderName = 64;
  static constexpr size_t kMaxHeaderValue = 4096;
  static constexpr size_t kMaxPathDepth = 8;
  static constexpr size_t kMaxFields = 24;
  static constexpr size_t kMaxNameBytes = 128;

  static size_t bodyCap(size_t requested, Operation op) {
    const size_t def = op == Operation::JsonGet ? kJsonGetDefault : kStreamDefault;
    const size_t hard = op == Operation::JsonGet ? kJsonGetHard : kStreamHard;
    if (requested == 0) return def;
    return requested > hard ? hard : requested;
  }

  static uint32_t timeoutMs(uint32_t requested) {
    if (requested == 0) return kTimeoutDefaultMs;
    if (requested < kTimeoutMinMs) return kTimeoutMinMs;
    return requested > kTimeoutMaxMs ? kTimeoutMaxMs : requested;
  }

  static bool validHeaders(const std::vector<std::pair<std::string, std::string>>& headers) {
    if (headers.size() > kMaxHeaders) return false;
    for (const auto& h : headers) {
      if (h.first.empty() || h.first.size() > kMaxHeaderName || h.second.size() > kMaxHeaderValue) {
        return false;
      }
      for (char c : h.first) if (static_cast<unsigned char>(c) <= 0x20 || c == ':') return false;
      for (char c : h.second) if (c == '\r' || c == '\n') return false;
    }
    return true;
  }

  static bool validJsonShape(const std::vector<std::string>& path,
                             const std::vector<std::string>& fields) {
    // An empty path addresses the JSON root object.  This is required for
    // APIs such as Jinjiang chapterContent whose response is
    // {"content":"..."}, rather than an array nested under a named key.
    // Keep fields mandatory so callers cannot request an unbounded projection.
    if (path.size() > kMaxPathDepth || fields.empty() || fields.size() > kMaxFields) {
      return false;
    }
    for (const auto& p : path) if (p.empty() || p.size() > kMaxNameBytes) return false;
    for (const auto& f : fields) if (f.empty() || f.size() > kMaxNameBytes) return false;
    return true;
  }
};

// Backend contract: publishPart() must preserve the old live file when it
// returns false. Implementations normally use live -> backup, part -> live,
// then remove backup, with rollback on failure.
class TransactionBackend {
 public:
  virtual ~TransactionBackend() = default;
  virtual bool openPart() = 0;
  virtual size_t writePart(const uint8_t* data, size_t len) = 0;
  virtual bool syncPart() = 0;
  virtual bool closePart() = 0;
  virtual bool partSize(size_t& sizeOut) = 0;
  virtual bool publishPart() = 0;
  virtual void discardPart() = 0;
};

class TransactionalWriter {
 public:
  explicit TransactionalWriter(TransactionBackend& backend) : backend_(backend) {}
  ~TransactionalWriter() {
    if (begun_ && !committed_) backend_.discardPart();
  }

  bool begin() {
    if (begun_) return false;
    begun_ = backend_.openPart();
    return begun_;
  }

  bool write(const uint8_t* data, size_t len) {
    if (!begun_ || committed_ || failed_) return false;
    if (len > static_cast<size_t>(-1) - bytes_) {
      failed_ = true;
      return false;
    }
    if (backend_.writePart(data, len) != len) {
      failed_ = true;
      return false;
    }
    bytes_ += len;
    return true;
  }

  bool commit() {
    if (!begun_ || committed_ || failed_) return false;
    size_t onDisk = 0;
    if (!backend_.syncPart() || !backend_.closePart() || !backend_.partSize(onDisk) ||
        onDisk != bytes_ || !backend_.publishPart()) {
      failed_ = true;
      backend_.discardPart();
      return false;
    }
    committed_ = true;
    return true;
  }

  void abort() {
    if (begun_ && !committed_) backend_.discardPart();
    failed_ = true;
  }

  size_t bytesWritten() const { return bytes_; }
  bool committed() const { return committed_; }

 private:
  TransactionBackend& backend_;
  size_t bytes_ = 0;
  bool begun_ = false;
  bool committed_ = false;
  bool failed_ = false;
};

class StreamSource {
 public:
  virtual ~StreamSource() = default;
  // >0 bytes, 0 temporarily unavailable, -1 clean EOF, <-1 source error.
  virtual int read(uint8_t* data, size_t capacity) = 0;
};

class StreamSink {
 public:
  virtual ~StreamSink() = default;
  virtual bool write(const uint8_t* data, size_t len) = 0;
};

enum class StreamError : uint8_t {
  None,
  Timeout,
  Cancelled,
  Source,
  Sink,
  TooLarge,
  Truncated,
};

struct StreamResult {
  StreamError error = StreamError::None;
  size_t bytes = 0;
  bool ok() const { return error == StreamError::None; }
};

struct StreamRuntime {
  uint32_t (*nowMs)() = nullptr;
  void (*wait)() = nullptr;
  bool (*cancelled)() = nullptr;
};

inline const char* streamErrorString(StreamError e) {
  switch (e) {
    case StreamError::None: return "";
    case StreamError::Timeout: return "timeout";
    case StreamError::Cancelled: return "cancelled";
    case StreamError::Source: return "network_read_failed";
    case StreamError::Sink: return "sink_write_failed";
    case StreamError::TooLarge: return "response_too_large";
    case StreamError::Truncated: return "network_truncated";
  }
  return "stream_failed";
}

// Constant-memory transfer used by all dl.* body paths. expectedSize ==
// SIZE_MAX / (size_t)-1 means an until-close/chunked body; otherwise early
// EOF is rejected.
//
// HTTP keep-alive + Transfer-Encoding: chunked is the common case for CDN
// bookstore APIs: after the last chunk is delivered, available()==0 but
// connected() stays true, so a pure "wait for disconnect" loop times out
// with a full body still buffered.  Treat ~400ms of quiet after progress as
// EOF for until-close transfers.
inline StreamResult stream(StreamSource& source, StreamSink& sink, size_t cap,
                           size_t expectedSize, uint32_t timeoutMs,
                           const StreamRuntime& runtime) {
  StreamResult out;
  uint8_t buf[2048];
  const uint32_t start = runtime.nowMs ? runtime.nowMs() : 0;
  uint32_t lastProgressMs = start;
  // Quiet window after last byte.  Too short → mid-transfer pause false EOF;
  // too long → slow UI.  CDN chunked keep-alive ends with quiet, but real
  // Wi-Fi transfers stall >1s (retransmits) — 700ms truncated the 45KB
  // jjwxc category JSON at 18KB (InvalidInput). 3s keeps correctness.
  constexpr uint32_t kIdleEofMs = 3000;
  for (;;) {
    if (runtime.cancelled && runtime.cancelled()) {
      out.error = StreamError::Cancelled;
      return out;
    }
    if (runtime.nowMs && static_cast<uint32_t>(runtime.nowMs() - start) >= timeoutMs) {
      out.error = StreamError::Timeout;
      return out;
    }
    if (expectedSize != static_cast<size_t>(-1) && out.bytes == expectedSize) return out;

    size_t want = sizeof(buf);
    if (out.bytes >= cap) {
      // Probe one byte to distinguish exact-cap EOF from overflow.
      const int n = source.read(buf, 1);
      if (n == -1) return out;
      if (n > 0) out.error = StreamError::TooLarge;
      else if (n < -1) out.error = StreamError::Source;
      else {
        if (runtime.wait) runtime.wait();
        continue;
      }
      return out;
    }
    if (want > cap - out.bytes) want = cap - out.bytes;
    if (expectedSize != static_cast<size_t>(-1) && want > expectedSize - out.bytes) {
      want = expectedSize - out.bytes;
    }
    const int n = source.read(buf, want);
    if (n > 0) {
      if (static_cast<size_t>(n) > want) {
        out.error = StreamError::Source;
        return out;
      }
      if (!sink.write(buf, static_cast<size_t>(n))) {
        out.error = StreamError::Sink;
        return out;
      }
      out.bytes += static_cast<size_t>(n);
      if (runtime.nowMs) lastProgressMs = runtime.nowMs();
      continue;
    }
    if (n == 0) {
      // Would-block / no data.  Keep-alive chunked bodies end with quiet
      // sockets that never disconnect — accept idle EOF after progress.
      if (expectedSize == static_cast<size_t>(-1) && out.bytes > 0 && runtime.nowMs &&
          static_cast<uint32_t>(runtime.nowMs() - lastProgressMs) >= kIdleEofMs) {
        return out;
      }
      if (runtime.wait) runtime.wait();
      continue;
    }
    if (n < -1) {
      out.error = StreamError::Source;
      return out;
    }
    if (expectedSize != static_cast<size_t>(-1) && out.bytes != expectedSize) {
      out.error = StreamError::Truncated;
    }
    return out;
  }
}

}  // namespace M4xHostIo

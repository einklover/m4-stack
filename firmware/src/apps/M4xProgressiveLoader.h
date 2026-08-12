#pragma once

// Some SdFat configurations leak a legacy `nullptr` macro through the
// global include graph.  It must not be visible while parsing C++ headers.
#ifdef nullptr
#undef nullptr
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

// Owner-task progressive loader: stream network → SD, early openText/openToc.
// HTTP session stays open across pump() slices so the UI task can launch the
// native reader while the remainder of the body is still downloading.

#include "apps/M4PluginReaderSession.h"
#include "apps/M4xJsonStream.h"
#include "util/M4PluginReaderBridge.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(ARDUINO_ARCH_ESP32)
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#endif

#include <SDCardManager.h>

// SdFat's legacy SysCall.h defines nullptr as a C macro.  That macro breaks
// the C++ standard library when this header is included before <memory>-using
// provider/catalog headers.  Keep the SDK include, but restore real C++11
// nullptr semantics for the rest of this translation unit.
#ifdef nullptr
#undef nullptr
#endif

namespace M4xProgressiveLoader {

enum class Kind : uint8_t { None, Chapter, Toc };
enum class Phase : uint8_t {
  Idle,
  CacheHit,
  Connecting,
  Streaming,
  EarlyOpened,
  Done,
  Failed,
};

struct Status {
  bool active = false;
  Kind kind = Kind::None;
  Phase phase = Phase::Idle;
  size_t bytes = 0;
  size_t rows = 0;
  bool early = false;
  bool done = false;
  const char* error = "";
  const char* path = "";
  // Human phase for plugin loading UI (never null).
  const char* phaseName = "idle";
};

struct ChapterSpec {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string relOut;
  std::string absOut;
  std::vector<std::string> jsonPath;  // empty = root
  std::string field;                  // e.g. "content"; empty = raw body
  bool rawBody = false;
  size_t earlyBytes = 2048;
  size_t maxBytes = 4 * 1024 * 1024;
  uint32_t timeoutMs = 30000;
  bool followRedirects = false;
  M4PluginReaderBridge::OpenRequest open;
};

struct TocSpec {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string relOut;
  std::string absOut;
  std::vector<std::string> jsonPath;
  std::vector<std::string> fields;
  size_t earlyRows = 40;
  size_t maxBytes = 8 * 1024 * 1024;
  uint32_t timeoutMs = 30000;
  bool followRedirects = false;
  int uidField0 = 0;
  int titleField0 = 1;
  int vipField0 = -1;  // optional VIP flag column for native TOC titles
  M4PluginReaderSession::TocRequest open;
};

// Live file sink (visible to reader mid-download).
class FileSink final : public M4xJsonStream::Sink {
 public:
  explicit FileSink(FsFile& f) : f_(f) {
#if defined(ARDUINO_ARCH_ESP32)
    buffer_ = static_cast<uint8_t*>(heap_caps_malloc(kBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
  }
  ~FileSink() override {
#if defined(ARDUINO_ARCH_ESP32)
    if (buffer_) heap_caps_free(buffer_);
#endif
  }
  bool write(const uint8_t* data, size_t len) override {
    if (!data || !len) return true;
    if (!buffer_) {
      const size_t n = f_.write(data, len);
      if (n != len) return false;
      written_ += n;
      return true;
    }
    while (len > 0) {
      const size_t n = std::min(len, kBufferBytes - used_);
      std::memcpy(buffer_ + used_, data, n);
      used_ += n;
      data += n;
      len -= n;
      written_ += n;
      if (used_ == kBufferBytes && !flushBuffer()) return false;
    }
    return true;
  }
  size_t written() const { return written_; }
  void forceFlush() {
    (void)flushBuffer();
    f_.flush();
  }

 private:
  static constexpr size_t kBufferBytes = 16 * 1024;
  bool flushBuffer() {
    if (!buffer_ || used_ == 0) return true;
    const size_t n = f_.write(buffer_, used_);
    if (n != used_) return false;
    used_ = 0;
    return true;
  }

  FsFile& f_;
  uint8_t* buffer_ = nullptr;
  size_t used_ = 0;
  size_t written_ = 0;
};

// Global single-job session (one plugin load at a time).
class Session {
 public:
  Status status() const;

  // Cache hit → queue open immediately. Miss → arm for pump().
  bool beginChapter(ChapterSpec spec, std::string& err);
  bool beginToc(TocSpec spec, std::string& err);

  // One cooperative slice. Returns true if more work remains.
  bool pump(uint32_t budgetMs, size_t budgetBytes);

  void cancel();
  bool active() const { return phase_ != Phase::Idle && phase_ != Phase::Done && phase_ != Phase::Failed; }
  bool needsPump() const {
    return phase_ == Phase::Connecting || phase_ == Phase::Streaming || phase_ == Phase::EarlyOpened;
  }

 private:
  enum class BodyMode : uint8_t { ContentLength, Chunked, UntilClose };
  // Cooperative chunked decoder (HTTPClient getStream is raw; CDN is often chunked).
  enum class ChunkPhase : uint8_t { SizeLine, Data, CrlfAfterData, Trailers, Done };

  void fail(const char* e);
  bool openOutFile(std::string& err);
  bool connectHttp(const std::string& url,
                   const std::vector<std::pair<std::string, std::string>>& headers, bool followRedirects,
                   std::string& err);
  void maybeEarlyChapter();
  void maybeEarlyToc();
  void publishTocRows(size_t rows);
  void finishOk();
  void closeHttp();
  std::string okMarkerPath() const;
  void writeOkMarker();
  void removeOutputs();  // incomplete chapter/toc + .ok (avoid cross-open stale cache)
  bool hasCompleteCache() const;
  // Feed already-decoded payload bytes into JSON/raw sink. Returns false on hard fail.
  bool acceptPayload(const uint8_t* data, size_t len);
  // Read up to want decoded payload bytes into out (handles chunked). Returns bytes decoded.
  // *more=false when body complete; *hardFail set on protocol error.
  size_t readDecoded(uint8_t* out, size_t want, bool* more, const char** hardFail);

  Phase phase_ = Phase::Idle;
  Kind kind_ = Kind::None;
  const char* error_ = "";
  size_t bytes_ = 0;       // wire or payload progress for status (decoded payload preferred)
  size_t rows_ = 0;
  bool early_ = false;
  std::string relOut_;
  std::string absOut_;
  size_t earlyThreshold_ = 0;
  size_t maxBytes_ = 0;
  uint32_t timeoutMs_ = 30000;
  uint32_t startMs_ = 0;
  bool rawBody_ = false;
  size_t lastPublishedRows_ = 0;

  BodyMode bodyMode_ = BodyMode::UntilClose;
  size_t contentRemain_ = 0;  // ContentLength only
  ChunkPhase chunkPhase_ = ChunkPhase::SizeLine;
  size_t chunkRemain_ = 0;
  std::string chunkLine_;
  bool bodyEof_ = false;
  bool streamComplete_ = false;  // true only after finishOk

  ChapterSpec chapter_;
  TocSpec toc_;

  FsFile outFile_;
  bool fileOpen_ = false;
  std::unique_ptr<FileSink> sink_;
  std::unique_ptr<M4xJsonStream::ScalarStreamExtractor> scalar_;
  std::unique_ptr<M4xJsonStream::RecordExtractor> records_;

#if defined(ARDUINO_ARCH_ESP32)
  std::unique_ptr<WiFiClientSecure> secure_;
  std::unique_ptr<HTTPClient> http_;
  WiFiClient* stream_ = nullptr;
#endif
};

Session& session();

}  // namespace M4xProgressiveLoader
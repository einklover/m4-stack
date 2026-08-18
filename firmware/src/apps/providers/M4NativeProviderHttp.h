#pragma once

#include "apps/M4xJsonStream.h"

#include <cstddef>
#include <cctype>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>

namespace M4NativeProviderHttp {

struct Header {
  std::string name;
  std::string value;
};

// Native provider requests never need an unbounded header list. Keeping the
// small list inline avoids vector growth/reallocation in the internal heap.
struct HeaderList {
  static constexpr size_t kMaxHeaders = 6;

  Header items[kMaxHeaders];
  size_t count = 0;

  void clear() { count = 0; }
  bool empty() const { return count == 0; }
  size_t size() const { return count; }

  bool push_back(Header header) {
    if (count >= kMaxHeaders) return false;
    items[count++] = std::move(header);
    return true;
  }

  HeaderList& operator=(std::initializer_list<Header> headers) {
    clear();
    for (const Header& header : headers) push_back(header);
    return *this;
  }

  Header* begin() { return items; }
  Header* end() { return items + count; }
  const Header* begin() const { return items; }
  const Header* end() const { return items + count; }
};

struct Request {
  std::string method = "GET";
  std::string url;
  HeaderList headers;
  std::string body;
  size_t maxBytes = 4u * 1024u * 1024u;
  uint32_t timeoutMs = 30000;
  bool followRedirects = false;
  // Public mirrors may opt out of CA validation. Credential-bearing providers
  // must leave this false.
  bool insecureTls = false;
};

struct Result {
  bool ok = false;
  int status = 0;
  size_t bytes = 0;
  std::string error;
  std::string location;
  HeaderList responseHeaders;
};

using ProgressFn = std::function<void(size_t bytes)>;
using CancelFn = std::function<bool()>;

// Bridges a streaming HTTP body into a ScalarStreamExtractor: requestToSink
// feeds raw bytes to the extractor, which parses the JSON and writes only the
// target scalar into its own Sink (file).
class ExtractorSink final : public M4xJsonStream::Sink {
 public:
  ExtractorSink(M4xJsonStream::ScalarStreamExtractor& extractor) : extractor_(extractor) {}
  bool write(const uint8_t* data, size_t len) override { return extractor_.feed(data, len); }

 private:
  M4xJsonStream::ScalarStreamExtractor& extractor_;
};

// Synchronous, bounded-body streaming request intended to run on the single
// native provider worker task. The body is never accumulated: HTTPClient
// decodes transfer framing and writes directly into SinkStream -> Sink.
Result requestToSink(const Request& req, M4xJsonStream::Sink& sink,
                     const ProgressFn& progress = {}, const CancelFn& cancelled = {});

// Small response helper for protocol metadata (psvts/login gate). Hard capped.
bool requestSmall(const Request& req, std::string& bodyOut, Result& resultOut,
                  size_t hardCap = 16u * 1024u, const CancelFn& cancelled = {});

}  // namespace M4NativeProviderHttp

#pragma once

#include "apps/providers/M4NovelProviderContract.h"

#include <cstddef>
#include <functional>
#include <string>

namespace M4NativeProviderBookDetail {

// Provider-neutral input for the ruleBookInfo stage. title/author are seeds
// already available from discovery/history and remain valid fallbacks when a
// remote detail endpoint omits optional metadata.
struct Request {
  std::string providerId;
  std::string appId;
  std::string bookId;
  std::string title;
  std::string author;
  size_t maxBytes = 96u * 1024u;
};

struct Result {
  bool ok = false;
  M4NovelProvider::BookDetail detail;
  size_t receivedBytes = 0;
  std::string error;
};

using CancelFn = std::function<bool()>;

// Synchronous bounded metadata fetch intended for a native provider worker.
// The implementation uses M4NativeProviderHttp only as a compatibility API;
// that layer is backed by M4HttpTransport/esp_http_client on current firmware.
Result fetch(const Request& req, const CancelFn& cancelled = {});

// Build the normalized fallback model without network I/O.
M4NovelProvider::BookDetail seed(const Request& req);

}  // namespace M4NativeProviderBookDetail

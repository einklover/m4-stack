#include "apps/M4xProgressiveLoader.h"

#include "apps/M4ContentProviderSession.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4PluginTocList.h"

#include <SDCardManager.h>

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#endif

namespace M4xProgressiveLoader {
namespace {

#if defined(ARDUINO_ARCH_ESP32)
uint32_t nowMs() { return millis(); }
#else
uint32_t nowMs() { return 0; }
#endif

}  // namespace

Session& session() {
  static Session s;
  return s;
}

Status Session::status() const {
  Status st;
  st.active = (phase_ != Phase::Idle && phase_ != Phase::Done && phase_ != Phase::Failed);
  st.kind = kind_;
  st.phase = phase_;
  st.bytes = bytes_;
  st.rows = rows_;
  st.early = early_;
  st.done = (phase_ == Phase::Done);
  st.error = error_ ? error_ : "";
  st.path = relOut_.c_str();
  switch (phase_) {
    case Phase::Idle:
      st.phaseName = "idle";
      break;
    case Phase::CacheHit:
      st.phaseName = "cache";
      break;
    case Phase::Connecting:
      st.phaseName = "connecting";
      break;
    case Phase::Streaming:
      st.phaseName = "streaming";
      break;
    case Phase::EarlyOpened:
      st.phaseName = "early";
      break;
    case Phase::Done:
      st.phaseName = "done";
      break;
    case Phase::Failed:
      st.phaseName = "failed";
      break;
  }
  return st;
}

std::string Session::okMarkerPath() const {
  if (absOut_.empty()) return {};
  return absOut_ + ".ok";
}

void Session::writeOkMarker() {
  const std::string ok = okMarkerPath();
  if (ok.empty()) return;
  FsFile f;
  if (!SdMan.openFileForWrite("LOAD", ok.c_str(), f)) return;
  const char one = '1';
  (void)f.write(reinterpret_cast<const uint8_t*>(&one), 1);
  f.close();
}

void Session::removeOutputs() {
  // Drop incomplete / failed body so the next open cannot cache-hit wrong text.
  if (!absOut_.empty() && SdMan.exists(absOut_.c_str())) {
    (void)SdMan.remove(absOut_.c_str());
  }
  const std::string ok = okMarkerPath();
  if (!ok.empty() && SdMan.exists(ok.c_str())) {
    (void)SdMan.remove(ok.c_str());
  }
}

bool Session::hasCompleteCache() const {
  if (absOut_.empty() || !SdMan.exists(absOut_.c_str())) return false;
  const std::string ok = okMarkerPath();
  if (ok.empty() || !SdMan.exists(ok.c_str())) return false;
  FsFile f;
  if (!SdMan.openFileForRead("LOAD", absOut_.c_str(), f)) return false;
  const size_t n = f.size();
  f.close();
  return n > 0;
}

void Session::fail(const char* e) {
  error_ = e ? e : "failed";
  phase_ = Phase::Failed;
  closeHttp();
  if (fileOpen_) {
    outFile_.close();
    fileOpen_ = false;
  }
  sink_.reset();
  scalar_.reset();
  records_.reset();
  // Incomplete download must not become a cache hit (wrong book / half chapter).
  // If we already early-opened, keep the partial for the live reader but strip .ok
  // so the next open re-fetches a full clean body.
  if (!streamComplete_) {
    if (!early_) {
      removeOutputs();
    } else {
      const std::string ok = okMarkerPath();
      if (!ok.empty() && SdMan.exists(ok.c_str())) (void)SdMan.remove(ok.c_str());
    }
  }
}

void Session::closeHttp() {
#if defined(ARDUINO_ARCH_ESP32)
  stream_ = nullptr;
  if (http_) {
    http_->end();
    http_.reset();
  }
  secure_.reset();
#endif
}

void Session::cancel() {
  if (phase_ == Phase::Idle) return;
  closeHttp();
  if (fileOpen_) {
    outFile_.close();
    fileOpen_ = false;
  }
  sink_.reset();
  scalar_.reset();
  records_.reset();
  // Cancel mid-flight: never leave a complete-looking cache for another book/chapter.
  if (!streamComplete_) {
    if (!early_) {
      removeOutputs();
    } else {
      const std::string ok = okMarkerPath();
      if (!ok.empty() && SdMan.exists(ok.c_str())) (void)SdMan.remove(ok.c_str());
    }
  }
  phase_ = Phase::Idle;
  kind_ = Kind::None;
  error_ = "cancelled";
  early_ = false;
  streamComplete_ = false;
  bytes_ = rows_ = 0;
}

bool Session::openOutFile(std::string& err) {
  // Ensure parent dirs exist (cache/<book>/...).
  {
    const size_t slash = absOut_.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
      const std::string dir = absOut_.substr(0, slash);
      if (!SdMan.exists(dir.c_str())) {
        // Best-effort nested mkdir.
        size_t pos = 1;
        while (pos < dir.size()) {
          const size_t n = dir.find('/', pos);
          const std::string sub = dir.substr(0, n == std::string::npos ? dir.size() : n);
          if (!sub.empty() && !SdMan.exists(sub.c_str())) SdMan.mkdir(sub.c_str());
          if (n == std::string::npos) break;
          pos = n + 1;
        }
      }
    }
  }
  if (!SdMan.openFileForWrite("LOAD", absOut_.c_str(), outFile_)) {
    err = "sd_open_failed";
    return false;
  }
  outFile_.seek(0);
  outFile_.truncate(0);
  fileOpen_ = true;
  sink_ = std::make_unique<FileSink>(outFile_);
  return true;
}

bool Session::connectHttp(const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          bool followRedirects, std::string& err) {
#if !defined(ARDUINO_ARCH_ESP32)
  (void)url;
  (void)headers;
  (void)followRedirects;
  err = "loader_device_only";
  return false;
#else
  secure_ = std::make_unique<WiFiClientSecure>();
  secure_->setInsecure();  // mirrors many public content CDN paths; cert bundle optional
  http_ = std::make_unique<HTTPClient>();
  // Keep the socket alive for the whole cooperative stream.  The same
  // HTTPClient is retained across pump() slices, so this avoids reconnecting
  // while the reader is already consuming the first bytes.
  http_->setReuse(true);
  http_->setTimeout(static_cast<int>(timeoutMs_));
  http_->setFollowRedirects(followRedirects ? HTTPC_FORCE_FOLLOW_REDIRECTS
                                            : HTTPC_DISABLE_FOLLOW_REDIRECTS);
  // Need Transfer-Encoding to decode chunked CDN bodies (晋江 app-cdn is chunked).
  static const char* kHdrKeys[] = {"Transfer-Encoding", "Content-Encoding", "Content-Type"};
  http_->collectHeaders(kHdrKeys, 3);
  if (!http_->begin(*secure_, url.c_str())) {
    err = "http_begin_failed";
    return false;
  }
  bool hasUA = false;
  bool hasAE = false;
  for (const auto& hv : headers) {
    if (!hv.first.empty()) {
      http_->addHeader(hv.first.c_str(), hv.second.c_str());
      if (hv.first == "User-Agent" || hv.first == "user-agent") hasUA = true;
      if (hv.first == "Accept-Encoding" || hv.first == "accept-encoding") hasAE = true;
    }
  }
  if (!hasUA) {
    http_->addHeader("User-Agent", "Mozilla/5.0 (Linux; Android 12) AppleWebKit/537.36 M4xApp/1.0");
  }
  // Avoid gzip on the progressive path (no inflate); prefer plain / chunked.
  if (!hasAE) http_->addHeader("Accept-Encoding", "identity");
  const int code = http_->GET();
  if (code != HTTP_CODE_OK) {
    err = (code < 0) ? "http_request_failed" : "http_error";
    // Diagnostic: serial is unreliable on M4; record internal heap state at
    // TLS handshake failure (jjwxc chapter/TOC fetch failing). Largest free
    // block tells us if the ~40KB mbedTLS handshake requirement was met.
#if defined(ARDUINO_ARCH_ESP32)
    FsFile df = SdMan.open("apps_data/com.jjwxc.client/logs/loader_heap.log",
                           O_WRONLY | O_CREAT | O_APPEND);
    if (df) {
      char line[160];
      const int n = snprintf(line, sizeof(line),
                             "[%lu] loader_fail err=%s code=%d ifree=%u largest=%u\n",
                             (unsigned long)millis(), err.c_str(), code,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      if (n > 0) df.write(reinterpret_cast<const uint8_t*>(line), (size_t)n);
      df.close();
    }
#endif
    return false;
  }
  const int cl = http_->getSize();
  if (cl > 0 && static_cast<size_t>(cl) > maxBytes_) {
    err = "response_too_large";
    return false;
  }
  // Detect body framing. getSize()>0 ⇒ Content-Length path.
  bodyMode_ = BodyMode::UntilClose;
  contentRemain_ = 0;
  chunkPhase_ = ChunkPhase::SizeLine;
  chunkRemain_ = 0;
  chunkLine_.clear();
  bodyEof_ = false;
  if (cl > 0) {
    bodyMode_ = BodyMode::ContentLength;
    contentRemain_ = static_cast<size_t>(cl);
  } else {
    const String te = http_->header("Transfer-Encoding");
    std::string teLower;
    teLower.reserve(te.length());
    for (unsigned i = 0; i < te.length(); ++i) {
      const char c = te[i];
      teLower.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c));
    }
    if (teLower.find("chunked") != std::string::npos) {
      bodyMode_ = BodyMode::Chunked;
    }
  }
  stream_ = http_->getStreamPtr();
  if (!stream_) {
    err = "no_stream";
    return false;
  }
#if defined(ARDUINO_ARCH_ESP32)
  Serial.printf("[LOADER] connected mode=%d cl=%d te=%s\n", static_cast<int>(bodyMode_), cl,
                http_->header("Transfer-Encoding").c_str());
#endif
  return true;
#endif
}

bool Session::acceptPayload(const uint8_t* data, size_t len) {
  if (!data || !len) return true;
  if (bytes_ + len > maxBytes_) {
    fail("response_too_large");
    return false;
  }
  bytes_ += len;
  if (kind_ == Kind::Chapter) {
    if (rawBody_) {
      if (!sink_ || !sink_->write(data, len)) {
        fail("sd_write_failed");
        return false;
      }
    } else if (scalar_) {
      if (!scalar_->feed(data, len)) {
        fail(M4xJsonStream::errorString(scalar_->error()));
        return false;
      }
    }
    maybeEarlyChapter();
  } else if (kind_ == Kind::Toc && records_) {
    if (!records_->feed(data, len)) {
      fail(M4xJsonStream::errorString(records_->error()));
      return false;
    }
    rows_ = records_->recordCount();
    maybeEarlyToc();
  }
  return true;
}

size_t Session::readDecoded(uint8_t* out, size_t want, bool* more, const char** hardFail) {
  *more = true;
  *hardFail = nullptr;
  if (!out || want == 0) return 0;
#if !defined(ARDUINO_ARCH_ESP32)
  *hardFail = "loader_device_only";
  *more = false;
  return 0;
#else
  if (!stream_) {
    *hardFail = "no_stream";
    *more = false;
    return 0;
  }
  if (bodyEof_) {
    *more = false;
    return 0;
  }

  size_t produced = 0;

  while (produced < want) {
    if (static_cast<uint32_t>(nowMs() - startMs_) >= timeoutMs_) {
      *hardFail = "timeout";
      *more = false;
      return produced;
    }

    if (bodyMode_ == BodyMode::ContentLength) {
      if (contentRemain_ == 0) {
        bodyEof_ = true;
        *more = false;
        break;
      }
      size_t n = want - produced;
      if (n > contentRemain_) n = contentRemain_;
      const int avail = stream_->available();
      if (avail <= 0) {
        if (!stream_->connected()) {
          *hardFail = "connection_closed";
          *more = false;
          break;
        }
        if (produced > 0) break;
        delay(1);
        if (stream_->available() <= 0) break;
        continue;
      }
      if (n > static_cast<size_t>(avail)) n = static_cast<size_t>(avail);
      const int r = stream_->read(out + produced, n);
      if (r <= 0) {
        if (!stream_->connected()) {
          *hardFail = "connection_closed";
          *more = false;
        }
        break;
      }
      produced += static_cast<size_t>(r);
      contentRemain_ -= static_cast<size_t>(r);
      if (contentRemain_ == 0) {
        bodyEof_ = true;
        *more = false;
      }
      continue;
    }

    if (bodyMode_ == BodyMode::UntilClose) {
      const int avail = stream_->available();
      if (avail <= 0) {
        if (!stream_->connected()) {
          bodyEof_ = true;
          *more = false;
          break;
        }
        if (produced > 0) break;
        delay(1);
        if (stream_->available() <= 0) break;
        continue;
      }
      size_t n = want - produced;
      if (n > static_cast<size_t>(avail)) n = static_cast<size_t>(avail);
      const int r = stream_->read(out + produced, n);
      if (r <= 0) {
        if (!stream_->connected()) {
          bodyEof_ = true;
          *more = false;
        }
        break;
      }
      produced += static_cast<size_t>(r);
      continue;
    }

    // Chunked
    if (chunkPhase_ == ChunkPhase::Done) {
      bodyEof_ = true;
      *more = false;
      break;
    }
    if (chunkPhase_ == ChunkPhase::SizeLine) {
      // Read until \n
      while (chunkLine_.size() < 32) {
        const int avail = stream_->available();
        if (avail <= 0) {
          if (!stream_->connected()) {
            *hardFail = "connection_closed";
            *more = false;
            return produced;
          }
          if (produced > 0) return produced;
          delay(1);
          if (stream_->available() <= 0) return produced;
          continue;
        }
        uint8_t c = 0;
        if (stream_->read(&c, 1) != 1) return produced;
        if (c == '\n') {
          if (!chunkLine_.empty() && chunkLine_.back() == '\r') chunkLine_.pop_back();
          // parse hex size
          size_t sz = 0;
          bool ok = false;
          for (char ch : chunkLine_) {
            if (ch == ';') break;
            size_t v = 0;
            if (ch >= '0' && ch <= '9')
              v = static_cast<size_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
              v = static_cast<size_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
              v = static_cast<size_t>(ch - 'A' + 10);
            else {
              ok = false;
              break;
            }
            sz = (sz << 4) + v;
            ok = true;
          }
          chunkLine_.clear();
          if (!ok && sz == 0) {
            // empty size line after previous? treat as error unless truly "0"
            *hardFail = "chunk_size_parse";
            *more = false;
            return produced;
          }
          // ok can be true for "0"
          if (!ok) {
            *hardFail = "chunk_size_parse";
            *more = false;
            return produced;
          }
          if (sz == 0) {
            chunkPhase_ = ChunkPhase::Trailers;
          } else {
            chunkRemain_ = sz;
            chunkPhase_ = ChunkPhase::Data;
          }
          break;
        }
        chunkLine_.push_back(static_cast<char>(c));
      }
      if (chunkLine_.size() >= 32) {
        *hardFail = "chunk_size_line";
        *more = false;
        return produced;
      }
      continue;
    }

    if (chunkPhase_ == ChunkPhase::Data) {
      if (chunkRemain_ == 0) {
        chunkPhase_ = ChunkPhase::CrlfAfterData;
        continue;
      }
      size_t n = want - produced;
      if (n > chunkRemain_) n = chunkRemain_;
      const int avail = stream_->available();
      if (avail <= 0) {
        if (!stream_->connected()) {
          *hardFail = "connection_closed";
          *more = false;
          return produced;
        }
        if (produced > 0) return produced;
        delay(1);
        if (stream_->available() <= 0) return produced;
        continue;
      }
      if (n > static_cast<size_t>(avail)) n = static_cast<size_t>(avail);
      const int r = stream_->read(out + produced, n);
      if (r <= 0) {
        if (!stream_->connected()) {
          *hardFail = "connection_closed";
          *more = false;
        }
        return produced;
      }
      produced += static_cast<size_t>(r);
      chunkRemain_ -= static_cast<size_t>(r);
      if (chunkRemain_ == 0) chunkPhase_ = ChunkPhase::CrlfAfterData;
      continue;
    }

    if (chunkPhase_ == ChunkPhase::CrlfAfterData) {
      uint8_t crlf[2];
      // need 2 bytes
      size_t got = 0;
      while (got < 2) {
        const int avail = stream_->available();
        if (avail <= 0) {
          if (!stream_->connected()) {
            *hardFail = "connection_closed";
            *more = false;
            return produced;
          }
          if (produced > 0) return produced;
          delay(1);
          if (stream_->available() <= 0) return produced;
          continue;
        }
        const int r = stream_->read(crlf + got, 2 - got);
        if (r <= 0) return produced;
        got += static_cast<size_t>(r);
      }
      if (crlf[0] != '\r' || crlf[1] != '\n') {
        *hardFail = "chunk_crlf_bad";
        *more = false;
        return produced;
      }
      chunkPhase_ = ChunkPhase::SizeLine;
      continue;
    }

    if (chunkPhase_ == ChunkPhase::Trailers) {
      // Read lines until empty line
      while (true) {
        if (chunkLine_.size() > 256) {
          *hardFail = "chunk_trailer";
          *more = false;
          return produced;
        }
        const int avail = stream_->available();
        if (avail <= 0) {
          if (!stream_->connected()) {
            // trailers optional; EOF ok
            chunkPhase_ = ChunkPhase::Done;
            bodyEof_ = true;
            *more = false;
            return produced;
          }
          if (produced > 0) return produced;
          delay(1);
          if (stream_->available() <= 0) return produced;
          continue;
        }
        uint8_t c = 0;
        if (stream_->read(&c, 1) != 1) return produced;
        if (c == '\n') {
          if (!chunkLine_.empty() && chunkLine_.back() == '\r') chunkLine_.pop_back();
          if (chunkLine_.empty()) {
            chunkPhase_ = ChunkPhase::Done;
            bodyEof_ = true;
            *more = false;
            return produced;
          }
          chunkLine_.clear();
          continue;
        }
        chunkLine_.push_back(static_cast<char>(c));
      }
    }
  }
  return produced;
#endif
}

void Session::maybeEarlyChapter() {
  if (early_ || kind_ != Kind::Chapter) return;
  const size_t n = sink_ ? sink_->written() : bytes_;
  // earlyBytes == 0 means "open only when stream is complete" (fanqie sets
  // this so partial pagination never freezes at ~few pages).  size_t n < 0 is
  // never true, so without this guard early_bytes=0 would fire at 1 byte.
  if (earlyThreshold_ == 0 && !streamComplete_ && phase_ != Phase::Done) return;
  if (n < earlyThreshold_ && !streamComplete_ && phase_ != Phase::Done) return;
  if (n < 1) return;
  if (sink_) sink_->forceFlush();
  chapter_.open.relPath = relOut_;
  chapter_.open.absPath = absOut_;
  // Mid-stream early open: pendingComplete so the native reader shows a
  // loading placeholder; finishOk re-opens with pendingComplete=false.
  // Complete-stream open (finishOk path): open real reader immediately.
  chapter_.open.pendingComplete = !streamComplete_;
  if (!M4PluginReaderSession::queueOpen(chapter_.open)) {
    // Keep streaming; plugin may retry open.
    return;
  }
  early_ = true;
  if (phase_ == Phase::Streaming) phase_ = Phase::EarlyOpened;
#if defined(ARDUINO_ARCH_ESP32)
  Serial.printf("[LOADER] early_chapter bytes=%u pending=%d path=%s\n", static_cast<unsigned>(n),
                chapter_.open.pendingComplete ? 1 : 0, relOut_.c_str());
#endif
}

void Session::publishTocRows(size_t rows) {
  if (kind_ != Kind::Toc || rows < 1) return;
  if (rows <= lastPublishedRows_) return;
  lastPublishedRows_ = rows;
  // Registry: plugins / resolveCatalogWork see the latest count.
  if (!toc_.open.providerId.empty() && !toc_.open.bookId.empty()) {
    M4ContentProvider::BookSpec spec;
    spec.providerId = toc_.open.providerId;
    spec.bookId = toc_.open.bookId;
    spec.title = toc_.open.bookTitle;
    spec.appId = toc_.open.appId;
    spec.currentIndex0 = toc_.open.currentIndex;
    spec.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;
    spec.catalog.fileRelPath = relOut_;
    spec.catalog.chapterCount = rows;
    spec.catalog.uidField0 = toc_.uidField0;
    spec.catalog.titleField0 = toc_.titleField0;
    spec.catalog.vipField0 = toc_.vipField0;
    (void)M4ContentProviderSession::registerBook(spec);
  }
  // Live native TOC picker (if already open after early handoff).
  (void)M4PluginTocList::publishLiveCatalogRowCount(rows);
}

void Session::maybeEarlyToc() {
  if (kind_ != Kind::Toc) return;
  if (records_) rows_ = records_->recordCount();
  if (rows_ < 1) return;
  // Always push growth to live picker / registry while streaming.
  if (sink_) sink_->forceFlush();
  publishTocRows(rows_);
  if (early_) return;
  if (rows_ < earlyThreshold_ && phase_ != Phase::Done) return;
  if (!M4PluginReaderSession::queueToc(toc_.open)) return;
  early_ = true;
  if (phase_ == Phase::Streaming) phase_ = Phase::EarlyOpened;
#if defined(ARDUINO_ARCH_ESP32)
  Serial.printf("[LOADER] early_toc rows=%u path=%s\n", static_cast<unsigned>(rows_), relOut_.c_str());
#endif
}

void Session::finishOk() {
  if (sink_) sink_->forceFlush();
  if (fileOpen_) {
    outFile_.close();
    fileOpen_ = false;
  }
  closeHttp();
  bytes_ = sink_ ? sink_->written() : bytes_;
  if (kind_ == Kind::Toc && records_) rows_ = records_->recordCount();
  // Final catalog size → live TOC picker grows to full list.
  if (kind_ == Kind::Toc) publishTocRows(rows_);
  // Body is complete before any open decision.  Must set this *before*
  // maybeEarlyChapter / final re-open: pendingComplete = !streamComplete_,
  // and the early→final branch used to check streamComplete_ while it was
  // still false, so every progressive chapter stuck on the "加载中…"
  // placeholder forever (both fanqie and jjwxc).
  streamComplete_ = true;
  if (!early_) {
    // Tiny chapter / short toc: first (and only) open with complete body.
    if (kind_ == Kind::Chapter) {
      earlyThreshold_ = 0;
      maybeEarlyChapter();
    } else if (kind_ == Kind::Toc) {
      earlyThreshold_ = 0;
      maybeEarlyToc();
    }
  } else if (kind_ == Kind::Chapter) {
    // Early open showed a loading placeholder (pendingComplete=true).
    // Re-open so the reader paginates the full file.
    chapter_.open.relPath = relOut_;
    chapter_.open.absPath = absOut_;
    chapter_.open.pendingComplete = false;
    if (M4PluginReaderSession::queueOpen(chapter_.open)) {
#if defined(ARDUINO_ARCH_ESP32)
      Serial.printf("[LOADER] final_open bytes=%u\n", static_cast<unsigned>(bytes_));
#endif
    }
  }
  writeOkMarker();  // only fully finished bodies are cache-eligible
  phase_ = Phase::Done;
  scalar_.reset();
  records_.reset();
  sink_.reset();
#if defined(ARDUINO_ARCH_ESP32)
  Serial.printf("[LOADER] done kind=%d bytes=%u rows=%u early=%d\n", static_cast<int>(kind_),
                static_cast<unsigned>(bytes_), static_cast<unsigned>(rows_), early_ ? 1 : 0);
#endif
}

bool Session::beginChapter(ChapterSpec spec, std::string& err) {
  cancel();
  chapter_ = std::move(spec);
  kind_ = Kind::Chapter;
  relOut_ = chapter_.relOut;
  absOut_ = chapter_.absOut;
  earlyThreshold_ = chapter_.earlyBytes;
  maxBytes_ = chapter_.maxBytes ? chapter_.maxBytes : 4 * 1024 * 1024;
  timeoutMs_ = chapter_.timeoutMs ? chapter_.timeoutMs : 30000;
  rawBody_ = chapter_.rawBody || chapter_.field.empty();
  error_ = "";
  early_ = false;
  streamComplete_ = false;
  bytes_ = rows_ = 0;
  startMs_ = nowMs();
  bodyMode_ = BodyMode::UntilClose;
  contentRemain_ = 0;
  chunkPhase_ = ChunkPhase::SizeLine;
  chunkRemain_ = 0;
  chunkLine_.clear();
  bodyEof_ = false;

  // Cache hit only when a previous stream fully finished (.ok marker).
  // Partial early-open leftovers must not open as "another book's chapter".
  if (hasCompleteCache()) {
    FsFile f;
    if (SdMan.openFileForRead("LOAD", absOut_.c_str(), f)) {
      const size_t n = f.size();
      f.close();
      if (n > 0) {
        bytes_ = n;
        phase_ = Phase::CacheHit;
        streamComplete_ = true;
        chapter_.open.relPath = relOut_;
        chapter_.open.absPath = absOut_;
        if (!M4PluginReaderSession::queueOpen(chapter_.open)) {
          err = "queue_open_failed";
          fail(err.c_str());
          return false;
        }
        early_ = true;
        phase_ = Phase::Done;
        return true;
      }
    }
  }

  // Stale partial without .ok: wipe before rewrite.
  removeOutputs();

  if (!openOutFile(err)) {
    fail(err.c_str());
    return false;
  }
  if (!rawBody_) {
    scalar_ = std::make_unique<M4xJsonStream::ScalarStreamExtractor>(chapter_.jsonPath, chapter_.field, *sink_);
  }
  // Defer TLS/HTTP connect to first pump() so Lua can paint "connecting…" first.
  // TLS is record-streamed (no need to download whole body before decrypt);
  // the handshake + TTFB still block one pump slice (~1–3s on weak Wi‑Fi).
  phase_ = Phase::Connecting;
  return true;
}

bool Session::beginToc(TocSpec spec, std::string& err) {
  cancel();
  toc_ = std::move(spec);
  kind_ = Kind::Toc;
  relOut_ = toc_.relOut;
  absOut_ = toc_.absOut;
  earlyThreshold_ = toc_.earlyRows;
  maxBytes_ = toc_.maxBytes ? toc_.maxBytes : 8 * 1024 * 1024;
  timeoutMs_ = toc_.timeoutMs ? toc_.timeoutMs : 30000;
  error_ = "";
  early_ = false;
  streamComplete_ = false;
  bytes_ = rows_ = 0;
  lastPublishedRows_ = 0;
  startMs_ = nowMs();
  bodyMode_ = BodyMode::UntilClose;
  contentRemain_ = 0;
  chunkPhase_ = ChunkPhase::SizeLine;
  chunkRemain_ = 0;
  chunkLine_.clear();
  bodyEof_ = false;

  if (hasCompleteCache()) {
    FsFile f;
    if (SdMan.openFileForRead("LOAD", absOut_.c_str(), f)) {
      // Count lines quickly for cache hit.
      size_t lines = 0;
      char buf[256];
      while (f.available()) {
        const int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i)
          if (buf[i] == '\n') ++lines;
      }
      const size_t n = f.size();
      f.close();
      if (n > 0 && lines > 0) {
        rows_ = lines;
        bytes_ = n;
        streamComplete_ = true;
        earlyThreshold_ = 0;
        maybeEarlyToc();
        if (!early_) {
          err = "queue_toc_failed";
          fail(err.c_str());
          return false;
        }
        phase_ = Phase::Done;
        return true;
      }
    }
  }

  removeOutputs();

  if (!openOutFile(err)) {
    fail(err.c_str());
    return false;
  }
  records_ = std::make_unique<M4xJsonStream::RecordExtractor>(toc_.jsonPath, toc_.fields, *sink_);
  // Defer connect — same as chapter (UI can show connecting before handshake).
  phase_ = Phase::Connecting;
  return true;
}

bool Session::pump(uint32_t budgetMs, size_t budgetBytes) {
  if (phase_ != Phase::Streaming && phase_ != Phase::EarlyOpened && phase_ != Phase::Connecting) {
    return false;
  }
#if !defined(ARDUINO_ARCH_ESP32)
  fail("loader_device_only");
  return false;
#else
  // Connecting slice: TLS handshake + HTTP headers only. Return so the host
  // can repaint progress before body streaming starts.
  if (phase_ == Phase::Connecting) {
    std::string err;
    const auto& url = (kind_ == Kind::Chapter) ? chapter_.url : toc_.url;
    const auto& headers = (kind_ == Kind::Chapter) ? chapter_.headers : toc_.headers;
    const bool follow = (kind_ == Kind::Chapter) ? chapter_.followRedirects : toc_.followRedirects;
    if (!connectHttp(url, headers, follow, err)) {
      fail(err.c_str());
      return false;
    }
    phase_ = Phase::Streaming;
    return true;
  }
  if (!stream_) {
    fail("no_stream");
    return false;
  }
  const uint32_t t0 = nowMs();
  size_t got = 0;
  uint8_t buf[1024];
  while (got < budgetBytes && static_cast<uint32_t>(nowMs() - t0) < budgetMs) {
    if (static_cast<uint32_t>(nowMs() - startMs_) >= timeoutMs_) {
      fail("timeout");
      return false;
    }
    bool more = true;
    const char* hard = nullptr;
    size_t want = sizeof(buf);
    if (want > budgetBytes - got) want = budgetBytes - got;
    const size_t n = readDecoded(buf, want, &more, &hard);
    if (hard) {
      fail(hard);
      return false;
    }
    if (n > 0) {
      if (!acceptPayload(buf, n)) return false;
      got += n;
    } else if (!more) {
      break;  // clean EOF of body framing
    } else {
      // No decoded bytes this slice (waiting on socket).
      return true;
    }
    if (!more) break;
  }

  // Body framing complete?
  if (bodyEof_) {
    if (kind_ == Kind::Chapter && !rawBody_ && scalar_) {
      if (!scalar_->finish()) {
        fail(M4xJsonStream::errorString(scalar_->error()));
        return false;
      }
      bytes_ = scalar_->bytesWritten();
    } else if (kind_ == Kind::Toc && records_) {
      if (!records_->finish()) {
        fail(M4xJsonStream::errorString(records_->error()));
        return false;
      }
      rows_ = records_->recordCount();
      bytes_ = sink_ ? sink_->written() : bytes_;
    } else if (kind_ == Kind::Chapter && rawBody_) {
      bytes_ = sink_ ? sink_->written() : bytes_;
    }
    if ((kind_ == Kind::Chapter && bytes_ < 1) || (kind_ == Kind::Toc && rows_ < 1)) {
      fail("empty");
      return false;
    }
    if (kind_ == Kind::Toc) {
      if (sink_) sink_->forceFlush();
      publishTocRows(rows_);
    }
    finishOk();
    return false;
  }
  // Mid-stream TOC growth (early already open).
  if (kind_ == Kind::Toc && early_ && records_) {
    rows_ = records_->recordCount();
    if (sink_) sink_->forceFlush();
    publishTocRows(rows_);
  }
  return true;  // more work
#endif
}

}  // namespace M4xProgressiveLoader

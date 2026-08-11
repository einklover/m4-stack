#pragma once

// Incremental JSON path/record extractor for large M4x API responses.
// Memory is bounded by one token and one output record; the response body is
// never materialized. Designed for direct network-chunk -> transactional sink.

#include "util/M4xJsonScan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace M4xJsonStream {

class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool write(const uint8_t* data, size_t len) = 0;
};

enum class Error : uint8_t {
  None,
  Syntax,
  Truncated,
  TokenTooLarge,
  TooManyRecords,
  SinkFailed,
  PathNotFound,
};

inline const char* errorString(Error e) {
  switch (e) {
    case Error::None: return "";
    case Error::Syntax: return "json_syntax";
    case Error::Truncated: return "json_truncated";
    case Error::TokenTooLarge: return "json_token_too_large";
    case Error::TooManyRecords: return "json_too_many_records";
    case Error::SinkFailed: return "sink_write_failed";
    case Error::PathNotFound: return "json_path_not_found";
  }
  return "json_error";
}

class RecordExtractor {
 public:
  static constexpr size_t kMaxTokenBytes = 8 * 1024;
  static constexpr size_t kMaxBufferedBytes = 24 * 1024;
  static constexpr size_t kMaxDepth = 48;
  static constexpr size_t kDefaultMaxRecords = 200000;

  // Empty path targets a root array. This is needed by compact APIs such as
  // JJWXC chapterList while keeping the same bounded streaming parser.
  RecordExtractor(std::vector<std::string> path, std::vector<std::string> fields, Sink& sink,
                  size_t maxRecords = kDefaultMaxRecords)
      : path_(std::move(path)), fields_(std::move(fields)), sink_(sink), maxRecords_(maxRecords) {
    if (fields_.empty() || fields_.size() > 24 || path_.size() > kMaxDepth || !trackMemory()) {
      error_ = Error::TokenTooLarge;
    }
  }

  bool feed(const uint8_t* data, size_t len) {
    if (error_ != Error::None || finished_) return false;
    for (size_t i = 0; i < len; ++i) {
      if (!consume(static_cast<char>(data[i]))) return false;
    }
    return true;
  }

  bool finish() {
    if (finished_) return error_ == Error::None;
    finished_ = true;
    if (error_ != Error::None) return false;
    if (lex_ != Lex::Normal || !frames_.empty() || !rootComplete_) {
      error_ = Error::Truncated;
      return false;
    }
    if (!targetSeen_) {
      error_ = Error::PathNotFound;
      return false;
    }
    return true;
  }

  Error error() const { return error_; }
  size_t recordCount() const { return recordCount_; }
  size_t peakBufferedBytes() const { return peakBuffered_; }

 private:
  enum class Kind : uint8_t { Object, Array };
  enum class ObjState : uint8_t { KeyOrEnd, Colon, Value, CommaOrEnd };
  enum class ArrState : uint8_t { ValueOrEnd, CommaOrEnd };
  enum class Lex : uint8_t { Normal, String, Primitive };
  static constexpr size_t kNoPath = static_cast<size_t>(-1);

  struct Frame {
    Kind kind = Kind::Object;
    ObjState obj = ObjState::KeyOrEnd;
    ArrState arr = ArrState::ValueOrEnd;
    size_t pathMatched = kNoPath;
    bool inTarget = false;
    bool record = false;
    bool afterComma = false;
    std::string key;
    std::vector<std::string> values;
  };

  static bool ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  size_t bufferedBytes() const {
    size_t n = token_.capacity() + frames_.capacity() * sizeof(Frame) +
               path_.capacity() * sizeof(std::string) + fields_.capacity() * sizeof(std::string);
    for (const auto& p : path_) n += p.capacity();
    for (const auto& f : fields_) n += f.capacity();
    for (const auto& f : frames_) {
      n += f.key.capacity();
      n += f.values.capacity() * sizeof(std::string);
      for (const auto& v : f.values) n += v.capacity();
    }
    return n;
  }

  bool trackMemory(size_t extra = 0) {
    const size_t now = bufferedBytes();
    if (extra > kMaxBufferedBytes || now > kMaxBufferedBytes - extra) {
      error_ = Error::TokenTooLarge;
      return false;
    }
    peakBuffered_ = std::max(peakBuffered_, now + extra);
    return true;
  }

  bool fail(Error e) {
    error_ = e;
    return false;
  }

  bool parentAcceptsValue() const {
    if (frames_.empty()) return !rootStarted_;
    const Frame& p = frames_.back();
    return p.kind == Kind::Object ? p.obj == ObjState::Value : p.arr == ArrState::ValueOrEnd;
  }

  void parentValueStarted() {
    if (frames_.empty()) {
      rootStarted_ = true;
      return;
    }
    Frame& p = frames_.back();
    if (p.kind == Kind::Object) p.obj = ObjState::CommaOrEnd;
    else p.arr = ArrState::CommaOrEnd;
    p.afterComma = false;
  }

  size_t childPathMatched(const Frame* parent) const {
    if (!parent) return 0;  // root container
    if (parent->kind == Kind::Object && parent->pathMatched != kNoPath &&
        parent->pathMatched < path_.size() && parent->key == path_[parent->pathMatched]) {
      return parent->pathMatched + 1;
    }
    // Arrays are transparent for path walking.
    if (parent->kind == Kind::Array) return parent->pathMatched;
    return kNoPath;
  }

  bool startContainer(Kind kind) {
    if (!parentAcceptsValue() || frames_.size() >= kMaxDepth) return fail(Error::Syntax);
    const Frame* parent = frames_.empty() ? nullptr : &frames_.back();
    const size_t matched = childPathMatched(parent);
    const bool inheritedTarget = parent && parent->inTarget;
    const bool beginsTarget = kind == Kind::Array && matched == path_.size();
    const bool isRecord = kind == Kind::Object && parent && parent->kind == Kind::Array &&
                          parent->inTarget;
    parentValueStarted();

    Frame f;
    f.kind = kind;
    f.pathMatched = matched;
    f.inTarget = inheritedTarget || beginsTarget;
    f.record = isRecord;
    if (f.record) f.values.resize(fields_.size());
    frames_.push_back(std::move(f));
    if (beginsTarget) targetSeen_ = true;
    return trackMemory();
  }

  bool closeContainer(Kind kind) {
    if (frames_.empty() || frames_.back().kind != kind) return fail(Error::Syntax);
    Frame& f = frames_.back();
    if (kind == Kind::Object && f.obj != ObjState::KeyOrEnd && f.obj != ObjState::CommaOrEnd) {
      return fail(Error::Syntax);
    }
    if (kind == Kind::Array && f.arr != ArrState::ValueOrEnd && f.arr != ArrState::CommaOrEnd) {
      return fail(Error::Syntax);
    }
    if (f.afterComma) return fail(Error::Syntax);
    if (f.record && !emitRecord(f)) return false;
    frames_.pop_back();
    if (frames_.empty()) rootComplete_ = true;
    return true;
  }

  static void appendRowValue(std::string& row, const std::string& value) {
    for (char c : value) {
      // The list-file format is TSV; prevent content from injecting rows or
      // columns while retaining UTF-8 bytes unchanged.
      row.push_back((c == '\t' || c == '\r' || c == '\n') ? ' ' : c);
    }
  }

  bool emitRecord(const Frame& f) {
    if (recordCount_ >= maxRecords_) return fail(Error::TooManyRecords);
    std::string row;
    size_t reserve = fields_.empty() ? 1 : fields_.size();
    for (const auto& v : f.values) reserve += v.size();
    if (reserve > kMaxTokenBytes * 2) return fail(Error::TokenTooLarge);
    row.reserve(reserve);
    for (size_t i = 0; i < f.values.size(); ++i) {
      if (i) row.push_back('\t');
      appendRowValue(row, f.values[i]);
    }
    row.push_back('\n');
    if (!trackMemory(row.capacity())) return false;
    if (!sink_.write(reinterpret_cast<const uint8_t*>(row.data()), row.size())) {
      return fail(Error::SinkFailed);
    }
    ++recordCount_;
    return true;
  }

  bool storeScalar(Frame& f, const std::string& value) {
    if (!f.record || f.kind != Kind::Object) return true;
    for (size_t i = 0; i < fields_.size(); ++i) {
      if (f.key == fields_[i]) {
        if (!trackMemory(value.size())) return false;
        f.values[i] = value;
        if (!trackMemory()) return false;
        break;
      }
    }
    return true;
  }

  bool scalar(const std::string& value, bool isString) {
    if (!parentAcceptsValue()) return fail(Error::Syntax);
    if (frames_.empty()) {
      rootStarted_ = rootComplete_ = true;
      return true;
    }
    Frame& f = frames_.back();
    if (f.kind == Kind::Object && (isString || value != "null") && !storeScalar(f, value)) return false;
    if (f.kind == Kind::Object) f.obj = ObjState::CommaOrEnd;
    else f.arr = ArrState::CommaOrEnd;
    f.afterComma = false;
    return true;
  }

  bool finishString() {
    std::string decoded;
    size_t end = 0;
    if (!M4xJsonScan::readString(token_, 0, decoded, end) || end != token_.size()) {
      return fail(Error::Syntax);
    }
    if (frames_.empty()) return scalar(decoded, true);
    Frame& f = frames_.back();
    if (f.kind == Kind::Object && f.obj == ObjState::KeyOrEnd) {
      f.key = std::move(decoded);
      f.obj = ObjState::Colon;
      f.afterComma = false;
      return trackMemory();
    }
    return scalar(decoded, true);
  }

  bool finishPrimitive() {
    if (token_.empty()) return fail(Error::Syntax);
    // Strict enough to reject arbitrary identifiers while accepting JSON
    // numbers and literals. ArduinoJson performs semantic parsing elsewhere.
    if (!validPrimitive(token_)) {
      return fail(Error::Syntax);
    }
    return scalar(token_, false);
  }

  // Validate a complete JSON primitive.  The incremental lexer deliberately
  // buffers until a delimiter, so this check must enforce RFC 8259 number
  // grammar rather than only looking at the first byte (which would accept
  // values such as `01`, `1.` and `1garbage`).
  static bool validPrimitive(const std::string& value) {
    if (value == "true" || value == "false" || value == "null") return true;
    if (value.empty()) return false;
    size_t i = 0;
    if (value[i] == '-') {
      if (++i == value.size()) return false;
    }
    if (value[i] == '0') {
      ++i;
      if (i < value.size() && value[i] >= '0' && value[i] <= '9') return false;
    } else {
      if (value[i] < '1' || value[i] > '9') return false;
      while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
    }
    if (i < value.size() && value[i] == '.') {
      ++i;
      const size_t frac = i;
      while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
      if (i == frac) return false;
    }
    if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
      ++i;
      if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
      const size_t exp = i;
      while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
      if (i == exp) return false;
    }
    return i == value.size();
  }

  bool consume(char c) {
    if (lex_ == Lex::String) {
      if (!stringEscaped_ && static_cast<unsigned char>(c) < 0x20) return fail(Error::Syntax);
      token_.push_back(c);
      if (token_.size() > kMaxTokenBytes) return fail(Error::TokenTooLarge);
      if (stringEscaped_) {
        stringEscaped_ = false;
      } else if (c == '\\') {
        stringEscaped_ = true;
      } else if (c == '"') {
        lex_ = Lex::Normal;
        return finishString();
      }
      return trackMemory();
    }
    if (lex_ == Lex::Primitive) {
      if (!ws(c) && c != ',' && c != ']' && c != '}') {
        token_.push_back(c);
        if (token_.size() > kMaxTokenBytes) return fail(Error::TokenTooLarge);
        return trackMemory();
      }
      lex_ = Lex::Normal;
      if (!finishPrimitive()) return false;
      // Delimiter belongs to the structural parser below.
    }

    if (ws(c)) return true;
    if (rootComplete_) return fail(Error::Syntax);
    switch (c) {
      case '{': return startContainer(Kind::Object);
      case '[': return startContainer(Kind::Array);
      case '}': return closeContainer(Kind::Object);
      case ']': return closeContainer(Kind::Array);
      case ':':
        if (frames_.empty() || frames_.back().kind != Kind::Object ||
            frames_.back().obj != ObjState::Colon) return fail(Error::Syntax);
        frames_.back().obj = ObjState::Value;
        return true;
      case ',':
        if (frames_.empty()) return fail(Error::Syntax);
        if (frames_.back().kind == Kind::Object) {
          if (frames_.back().obj != ObjState::CommaOrEnd) return fail(Error::Syntax);
          frames_.back().obj = ObjState::KeyOrEnd;
          frames_.back().key.clear();
          frames_.back().afterComma = true;
        } else {
          if (frames_.back().arr != ArrState::CommaOrEnd) return fail(Error::Syntax);
          frames_.back().arr = ArrState::ValueOrEnd;
          frames_.back().afterComma = true;
        }
        return true;
      case '"':
        token_.assign(1, '"');
        lex_ = Lex::String;
        stringEscaped_ = false;
        return true;
      default:
        if (!parentAcceptsValue()) return fail(Error::Syntax);
        token_.assign(1, c);
        lex_ = Lex::Primitive;
        return true;
    }
  }

  std::vector<std::string> path_;
  std::vector<std::string> fields_;
  Sink& sink_;
  size_t maxRecords_;
  std::vector<Frame> frames_;
  std::string token_;
  Lex lex_ = Lex::Normal;
  Error error_ = Error::None;
  size_t recordCount_ = 0;
  size_t peakBuffered_ = 0;
  bool stringEscaped_ = false;
  bool rootStarted_ = false;
  bool rootComplete_ = false;
  bool targetSeen_ = false;
  bool finished_ = false;
};

class ScalarStreamExtractor {
 public:
  ScalarStreamExtractor(std::vector<std::string> path, std::string field, Sink& sink)
      : path_(std::move(path)), field_(std::move(field)), sink_(sink) {
    if (field_.empty() || path_.size() > kMaxDepth) error_ = Error::TokenTooLarge;
  }

  bool feed(const uint8_t* data, size_t len) {
    if (error_ != Error::None || finished_) return false;
    for (size_t i = 0; i < len; ++i) {
      if (!consume(static_cast<char>(data[i]))) return false;
    }
    return true;
  }

  bool finish() {
    if (finished_) return error_ == Error::None;
    finished_ = true;
    if (error_ != Error::None) return false;
    if (streamMode_ || lex_ != Lex::Normal) {
      error_ = Error::Truncated;
      return false;
    }
    if (!fieldSeen_) {
      error_ = Error::PathNotFound;
      return false;
    }
    return true;
  }

  Error error() const { return error_; }
  size_t bytesWritten() const { return bytesWritten_; }
  bool fieldSeen() const { return fieldSeen_; }

 private:
  static constexpr size_t kMaxDepth = 48;
  static constexpr size_t kMaxKeyBytes = 256;
  // String = short keys only (buffered). SkipString = non-target values discarded
  // without a size cap (晋江 sayBody/sayBodyV2 常 >256B 且排在 content 之前).
  enum class Lex : uint8_t { Normal, String, SkipString, Primitive, StreamString };
  enum class Expect : uint8_t { Value, KeyOrEnd, Colon, ValueAfterColon, CommaOrEnd };

  static bool ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  bool fail(Error e) {
    error_ = e;
    return false;
  }

  bool pathMatches() const {
    return depthMatched_ == path_.size();
  }

  bool writeByte(uint8_t b) {
    if (!sink_.write(&b, 1)) return fail(Error::SinkFailed);
    ++bytesWritten_;
    return true;
  }

  // Emit a Unicode code point as UTF-8 (BMP + supplementary via surrogates).
  bool writeCodePoint(uint32_t cp) {
    if (cp > 0x10FFFF) cp = 0xFFFD;
    uint8_t buf[4];
    size_t n = 0;
    if (cp < 0x80) {
      buf[n++] = static_cast<uint8_t>(cp);
    } else if (cp < 0x800) {
      buf[n++] = static_cast<uint8_t>(0xC0 | (cp >> 6));
      buf[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      buf[n++] = static_cast<uint8_t>(0xE0 | (cp >> 12));
      buf[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      buf[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    } else {
      buf[n++] = static_cast<uint8_t>(0xF0 | (cp >> 18));
      buf[n++] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
      buf[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      buf[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    }
    for (size_t i = 0; i < n; ++i) {
      if (!writeByte(buf[i])) return false;
    }
    return true;
  }

  static int hexNibble(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
  }

  // Streaming JSON string unescape. 晋江/番茄 content 常为 \uXXXX 转义中文;
  // 绝不能写成 '?'，否则整章全是问号。
  bool streamStringChar(char c) {
    // Collecting \uXXXX hex digits.
    if (unicodeLeft_ > 0) {
      const int nib = hexNibble(c);
      if (nib < 0) return fail(Error::Syntax);
      unicodeCode_ = (unicodeCode_ << 4) | static_cast<uint32_t>(nib);
      --unicodeLeft_;
      if (unicodeLeft_ > 0) return true;

      // Four hex digits complete.
      if (unicodeHi_ != 0) {
        // Expected low surrogate after high.
        const uint32_t lo = unicodeCode_;
        uint32_t cp = 0xFFFD;
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((unicodeHi_ - 0xD800) << 10) + (lo - 0xDC00);
        }
        unicodeHi_ = 0;
        unicodeCode_ = 0;
        return writeCodePoint(cp);
      }
      if (unicodeCode_ >= 0xD800 && unicodeCode_ <= 0xDBFF) {
        // High surrogate: remember and wait for next \u.
        unicodeHi_ = unicodeCode_;
        unicodeCode_ = 0;
        return true;
      }
      if (unicodeCode_ >= 0xDC00 && unicodeCode_ <= 0xDFFF) {
        unicodeCode_ = 0xFFFD;  // lone low surrogate
      }
      const uint32_t cp = unicodeCode_;
      unicodeCode_ = 0;
      return writeCodePoint(cp);
    }

    // Pending high surrogate without following \u yet.
    if (unicodeHi_ != 0) {
      if (esc_) {
        esc_ = false;
        if (c == 'u') {
          unicodeLeft_ = 4;
          unicodeCode_ = 0;
          return true;
        }
        // Not \u — flush high as replacement, then handle this escape.
        if (!writeCodePoint(0xFFFD)) return false;
        unicodeHi_ = 0;
        // re-handle as normal escape for `c`
        if (c == 'u') {
          unicodeLeft_ = 4;
          unicodeCode_ = 0;
          return true;
        }
        char out = c;
        switch (c) {
          case '"':
          case '\\':
          case '/':
            out = c;
            break;
          case 'b':
            out = '\b';
            break;
          case 'f':
            out = '\f';
            break;
          case 'n':
            out = '\n';
            break;
          case 'r':
            out = '\r';
            break;
          case 't':
            out = '\t';
            break;
          default:
            break;
        }
        return writeByte(static_cast<uint8_t>(out));
      }
      if (c == '\\') {
        esc_ = true;
        return true;
      }
      // Flush orphan high surrogate.
      if (!writeCodePoint(0xFFFD)) return false;
      unicodeHi_ = 0;
      // fall through to normal char handling
    }

    if (esc_) {
      esc_ = false;
      if (c == 'u') {
        unicodeLeft_ = 4;
        unicodeCode_ = 0;
        return true;
      }
      char out = c;
      switch (c) {
        case '"':
        case '\\':
        case '/':
          out = c;
          break;
        case 'b':
          out = '\b';
          break;
        case 'f':
          out = '\f';
          break;
        case 'n':
          out = '\n';
          break;
        case 'r':
          out = '\r';
          break;
        case 't':
          out = '\t';
          break;
        default:
          out = c;
          break;
      }
      return writeByte(static_cast<uint8_t>(out));
    }
    if (c == '\\') {
      esc_ = true;
      return true;
    }
    if (c == '"') {
      if (unicodeHi_ != 0) {
        unicodeHi_ = 0;
        if (!writeCodePoint(0xFFFD)) return false;
      }
      streamMode_ = false;
      lex_ = Lex::Normal;
      fieldSeen_ = true;
      expect_ = Expect::CommaOrEnd;
      return true;
    }
    if (static_cast<unsigned char>(c) < 0x20) return fail(Error::Syntax);
    // Literal UTF-8 multi-byte sequences pass through unchanged.
    return writeByte(static_cast<uint8_t>(c));
  }

  bool onStringFinished() {
    // Key or non-target string value finished into token_ (includes quotes).
    std::string decoded;
    size_t end = 0;
    if (!M4xJsonScan::readString(token_, 0, decoded, end) || end != token_.size()) {
      return fail(Error::Syntax);
    }
    if (expect_ == Expect::KeyOrEnd) {
      pendingKey_ = std::move(decoded);
      expect_ = Expect::Colon;
      return true;
    }
    // Non-streamed string value (not our field, or not at target depth).
    expect_ = Expect::CommaOrEnd;
    return true;
  }

  bool consume(char c) {
    if (lex_ == Lex::StreamString) return streamStringChar(c);

    // Discard non-target string values (any length). Only track escapes + closing quote.
    if (lex_ == Lex::SkipString) {
      if (!stringEscaped_ && static_cast<unsigned char>(c) < 0x20) return fail(Error::Syntax);
      if (stringEscaped_) {
        stringEscaped_ = false;
        return true;
      }
      if (c == '\\') {
        stringEscaped_ = true;
        return true;
      }
      if (c == '"') {
        lex_ = Lex::Normal;
        expect_ = Expect::CommaOrEnd;
        return true;
      }
      return true;
    }

    if (lex_ == Lex::String) {
      if (!stringEscaped_ && static_cast<unsigned char>(c) < 0x20) return fail(Error::Syntax);
      token_.push_back(c);
      if (token_.size() > kMaxKeyBytes + 2) return fail(Error::TokenTooLarge);
      if (stringEscaped_) {
        stringEscaped_ = false;
      } else if (c == '\\') {
        stringEscaped_ = true;
      } else if (c == '"') {
        lex_ = Lex::Normal;
        return onStringFinished();
      }
      return true;
    }

    if (lex_ == Lex::Primitive) {
      if (!ws(c) && c != ',' && c != ']' && c != '}') {
        token_.push_back(c);
        if (token_.size() > 64) return fail(Error::TokenTooLarge);
        return true;
      }
      lex_ = Lex::Normal;
      expect_ = Expect::CommaOrEnd;
      // fall through with delimiter
    }

    if (ws(c)) return true;

    switch (c) {
      case '{':
        ++objDepth_;
        if (static_cast<size_t>(objDepth_) > kMaxDepth) return fail(Error::TokenTooLarge);
        // Enter next path segment if key matched
        if (!path_.empty() && depthMatched_ < path_.size() && keyForPath_ &&
            pendingKey_ == path_[depthMatched_]) {
          ++depthMatched_;
        }
        keyForPath_ = false;
        pendingKey_.clear();
        expect_ = Expect::KeyOrEnd;
        return true;
      case '}':
        if (objDepth_ <= 0) return fail(Error::Syntax);
        --objDepth_;
        if (depthMatched_ > 0 && static_cast<size_t>(objDepth_) < depthMatched_) {
          // Left a matched path frame
          if (depthMatched_ > 0) --depthMatched_;
        }
        expect_ = Expect::CommaOrEnd;
        return true;
      case '[':
        ++arrDepth_;
        expect_ = Expect::Value;
        return true;
      case ']':
        if (arrDepth_ <= 0) return fail(Error::Syntax);
        --arrDepth_;
        expect_ = Expect::CommaOrEnd;
        return true;
      case ':':
        if (expect_ != Expect::Colon) return fail(Error::Syntax);
        // About to read value for pendingKey_
        if (pathMatches() && pendingKey_ == field_) {
          wantStreamValue_ = true;
        } else {
          wantStreamValue_ = false;
          keyForPath_ = true;  // value may be object for path
        }
        expect_ = Expect::ValueAfterColon;
        return true;
      case ',':
        expect_ = (arrDepth_ > 0 && objDepth_ == 0) ? Expect::Value : Expect::KeyOrEnd;
        pendingKey_.clear();
        wantStreamValue_ = false;
        return true;
      case '"':
        if (wantStreamValue_ && expect_ == Expect::ValueAfterColon) {
          // Start streaming field body (no opening quote written).
          wantStreamValue_ = false;
          streamMode_ = true;
          lex_ = Lex::StreamString;
          esc_ = false;
          unicodeLeft_ = 0;
          unicodeCode_ = 0;
          unicodeHi_ = 0;
          return true;
        }
        // Object keys: buffer (short). Non-target string values: skip (unbounded).
        if (expect_ == Expect::KeyOrEnd) {
          token_.assign(1, '"');
          lex_ = Lex::String;
          stringEscaped_ = false;
          return true;
        }
        // Value string that is not our target field (sayBody, messages, …).
        lex_ = Lex::SkipString;
        stringEscaped_ = false;
        return true;
      default:
        if (expect_ != Expect::Value && expect_ != Expect::ValueAfterColon) return fail(Error::Syntax);
        wantStreamValue_ = false;
        token_.assign(1, c);
        lex_ = Lex::Primitive;
        return true;
    }
  }

  std::vector<std::string> path_;
  std::string field_;
  Sink& sink_;
  Error error_ = Error::None;
  Lex lex_ = Lex::Normal;
  Expect expect_ = Expect::Value;
  std::string token_;
  std::string pendingKey_;
  bool stringEscaped_ = false;
  bool streamMode_ = false;
  bool esc_ = false;
  int unicodeLeft_ = 0;
  uint32_t unicodeCode_ = 0;
  uint32_t unicodeHi_ = 0;  // pending high surrogate, 0 if none
  bool wantStreamValue_ = false;
  bool keyForPath_ = false;
  bool fieldSeen_ = false;
  bool finished_ = false;
  int objDepth_ = 0;
  int arrDepth_ = 0;
  size_t depthMatched_ = 0;
  size_t bytesWritten_ = 0;
};


}  // namespace M4xJsonStream

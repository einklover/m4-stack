#pragma once

// Bounded-memory, seekable line/page source for host-owned plugin lists.
//
// The source performs one constant-memory scan at open time to establish an
// accurate row/page count.  It never keeps all rows (or all row offsets) in
// memory: only a bounded set of page cursors is cached, and readPage() returns
// at most one page of strings.  The storage adapter is deliberately abstract
// so production can wrap FsFile while simulator tests use an in-memory file.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace M4FileRows {

class ISeekableInput {
 public:
  virtual ~ISeekableInput() = default;
  virtual uint64_t size() const = 0;
  virtual bool seek(uint64_t offset) = 0;
  // Returns bytes read, 0 at EOF, or a negative value on I/O failure.
  virtual int64_t read(uint8_t* dst, size_t capacity) = 0;
};

struct Limits {
  uint64_t maxFileBytes = 8u * 1024u * 1024u;
  size_t maxRows = 200000;
  size_t maxLineBytes = 2048;
  int maxPageSize = 32;
  size_t maxCursors = 12;
  size_t scanChunkBytes = 1024;
};

enum class Error : uint8_t {
  None = 0,
  NoInput,
  InvalidLimits,
  FileTooLarge,
  TooManyRows,
  LineTooLong,
  SeekFailed,
  ReadFailed,
  NotOpen,
};

const char* errorKey(Error error);

struct Row {
  size_t index0 = 0;
  std::string line;
};

struct PageResult {
  Error error = Error::None;
  int page = 1;
  int totalPages = 1;
  size_t totalRows = 0;
  bool hasPrevious = false;
  bool hasNext = false;
  std::vector<Row> rows;

  explicit operator bool() const { return error == Error::None; }
};

class FileRowSource {
 public:
  FileRowSource() = default;
  ~FileRowSource() = default;
  FileRowSource(const FileRowSource&) = delete;
  FileRowSource& operator=(const FileRowSource&) = delete;

  // When knownRowCount is non-zero, skip the metadata title/row scan and use
  // the provider's already-validated catalog count.  Rows are still read and
  // bounded one page at a time.  This is the fast path used by the native
  // system chapter list; legacy callers can leave it at zero to retain the
  // fully self-describing scan.
  Error open(std::unique_ptr<ISeekableInput> input, int pageSize, Limits limits = {},
             size_t knownRowCount = 0);
  void close();
  bool isOpen() const { return input_ != nullptr && openError_ == Error::None; }

  PageResult readPage(int requestedPage);

  size_t rowCount() const { return rowCount_; }
  int pageCount() const { return totalPages_; }
  int pageSize() const { return pageSize_; }
  size_t cursorCount() const { return cursors_.size(); }
  size_t maxCursorCount() const { return limits_.maxCursors; }
  size_t residentIndexBytes() const {
    return buffer_.capacity() * sizeof(uint8_t) + cursors_.capacity() * sizeof(Cursor);
  }
  Error openError() const { return openError_; }

 private:
  struct Cursor {
    size_t rowIndex0 = 0;
    uint64_t offset = 0;
    uint64_t stamp = 0;
    bool pinned = false;
  };

  Error scanMetadata();
  Error seekToPage(int page, uint64_t& offsetOut);
  Error readLine(std::string& lineOut, bool& hadRow, uint64_t& logicalOffset);
  void rememberCursor(int page, uint64_t offset);
  const Cursor* bestCursor(size_t targetRowIndex0) const;

  std::unique_ptr<ISeekableInput> input_;
  Limits limits_{};
  int pageSize_ = 1;
  int totalPages_ = 1;
  size_t rowCount_ = 0;
  Error openError_ = Error::NotOpen;
  std::vector<Cursor> cursors_;
  uint64_t stamp_ = 0;

  // Buffered reader state, reset on every explicit seek.
  std::vector<uint8_t> buffer_;
  size_t bufferPos_ = 0;
  size_t bufferLen_ = 0;
};

}  // namespace M4FileRows

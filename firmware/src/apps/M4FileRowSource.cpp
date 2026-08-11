#include "apps/M4FileRowSource.h"

#include <algorithm>
#include <limits>

namespace M4FileRows {

const char* errorKey(Error error) {
  switch (error) {
    case Error::None:
      return "ok";
    case Error::NoInput:
      return "no_input";
    case Error::InvalidLimits:
      return "invalid_limits";
    case Error::FileTooLarge:
      return "file_too_large";
    case Error::TooManyRows:
      return "too_many_rows";
    case Error::LineTooLong:
      return "line_too_long";
    case Error::SeekFailed:
      return "seek_failed";
    case Error::ReadFailed:
      return "read_failed";
    case Error::NotOpen:
    default:
      return "not_open";
  }
}

Error FileRowSource::open(std::unique_ptr<ISeekableInput> input, int pageSize, Limits limits,
                          size_t knownRowCount) {
  close();
  if (!input) {
    openError_ = Error::NoInput;
    return openError_;
  }
  if (limits.maxFileBytes == 0 || limits.maxRows == 0 || limits.maxLineBytes == 0 ||
      limits.maxPageSize < 1 || limits.maxCursors < 2 || limits.scanChunkBytes < 64) {
    openError_ = Error::InvalidLimits;
    return openError_;
  }
  if (pageSize < 1 || pageSize > limits.maxPageSize) {
    openError_ = Error::InvalidLimits;
    return openError_;
  }
  input_ = std::move(input);
  limits_ = limits;
  pageSize_ = pageSize;
  buffer_.resize(limits_.scanChunkBytes);
  if (knownRowCount > limits_.maxRows) {
    openError_ = Error::TooManyRows;
  } else if (knownRowCount > 0) {
    // The provider registry has already validated this count.  Do not scan
    // every title before showing the first system-list frame.
    rowCount_ = knownRowCount;
    openError_ = input_->size() > limits_.maxFileBytes
                     ? Error::FileTooLarge
                     : (input_->seek(0) ? Error::None : Error::SeekFailed);
  } else {
    openError_ = scanMetadata();
  }
  if (openError_ != Error::None) {
    input_.reset();
    buffer_.clear();
    return openError_;
  }
  totalPages_ = rowCount_ == 0
                    ? 1
                    : static_cast<int>((rowCount_ + static_cast<size_t>(pageSize_) - 1) /
                                       static_cast<size_t>(pageSize_));
  cursors_.reserve(limits_.maxCursors);
  rememberCursor(1, 0);
  return Error::None;
}

void FileRowSource::close() {
  input_.reset();
  buffer_.clear();
  bufferPos_ = 0;
  bufferLen_ = 0;
  cursors_.clear();
  rowCount_ = 0;
  pageSize_ = 1;
  totalPages_ = 1;
  stamp_ = 0;
  openError_ = Error::NotOpen;
}

Error FileRowSource::scanMetadata() {
  if (!input_) return Error::NoInput;
  const uint64_t bytes = input_->size();
  if (bytes > limits_.maxFileBytes) return Error::FileTooLarge;
  if (!input_->seek(0)) return Error::SeekFailed;

  rowCount_ = 0;
  size_t lineBytes = 0;
  bool sawAny = false;
  uint64_t absoluteOffset = 0;
  size_t sparseStrideRows = static_cast<size_t>(pageSize_);
  const size_t sparseCapacity = std::max<size_t>(2, limits_.maxCursors / 2);
  cursors_.clear();
  cursors_.push_back(Cursor{0, 0, ++stamp_, true});
  while (true) {
    const int64_t n = input_->read(buffer_.data(), buffer_.size());
    if (n < 0) return Error::ReadFailed;
    if (n == 0) break;
    sawAny = true;
    for (int64_t i = 0; i < n; ++i) {
      const uint8_t c = buffer_[static_cast<size_t>(i)];
      ++absoluteOffset;
      if (c == '\n') {
        if (++rowCount_ > limits_.maxRows) return Error::TooManyRows;
        lineBytes = 0;
        if (rowCount_ % sparseStrideRows == 0) {
          if (cursors_.size() >= sparseCapacity) {
            sparseStrideRows *= 2;
            cursors_.erase(
                std::remove_if(cursors_.begin() + 1, cursors_.end(),
                               [&](const Cursor& cursor) {
                                 return cursor.rowIndex0 % sparseStrideRows != 0;
                               }),
                cursors_.end());
          }
          if (rowCount_ % sparseStrideRows == 0 && cursors_.size() < sparseCapacity) {
            cursors_.push_back(Cursor{rowCount_, absoluteOffset, ++stamp_, true});
          }
        }
      } else {
        if (++lineBytes > limits_.maxLineBytes) return Error::LineTooLong;
      }
    }
  }
  // A trailing newline terminates the final row; it does not create another
  // empty row. A non-newline final byte does.
  if (sawAny && lineBytes > 0 && ++rowCount_ > limits_.maxRows) return Error::TooManyRows;
  if (!input_->seek(0)) return Error::SeekFailed;
  bufferPos_ = 0;
  bufferLen_ = 0;
  return Error::None;
}

const FileRowSource::Cursor* FileRowSource::bestCursor(size_t targetRowIndex0) const {
  const Cursor* best = nullptr;
  for (const Cursor& c : cursors_) {
    if (c.rowIndex0 <= targetRowIndex0 && (!best || c.rowIndex0 > best->rowIndex0)) best = &c;
  }
  return best;
}

void FileRowSource::rememberCursor(int page, uint64_t offset) {
  if (page < 1 || page > totalPages_) return;
  const size_t rowIndex0 = static_cast<size_t>(page - 1) * static_cast<size_t>(pageSize_);
  for (Cursor& c : cursors_) {
    if (c.rowIndex0 == rowIndex0) {
      c.offset = offset;
      c.stamp = ++stamp_;
      return;
    }
  }
  Cursor next{rowIndex0, offset, ++stamp_, false};
  if (cursors_.size() < limits_.maxCursors) {
    cursors_.push_back(next);
    return;
  }
  // Metadata-scan anchors are pinned. Evict the least-recently-used runtime
  // cursor so random access cannot destroy the bounded sparse index.
  auto victim = cursors_.end();
  for (auto it = cursors_.begin(); it != cursors_.end(); ++it) {
    if (it->pinned) continue;
    if (victim == cursors_.end() || it->stamp < victim->stamp) victim = it;
  }
  if (victim != cursors_.end()) *victim = next;
}

Error FileRowSource::readLine(std::string& lineOut, bool& hadRow, uint64_t& logicalOffset) {
  lineOut.clear();
  hadRow = false;
  while (true) {
    if (bufferPos_ == bufferLen_) {
      const int64_t n = input_->read(buffer_.data(), buffer_.size());
      if (n < 0) return Error::ReadFailed;
      bufferPos_ = 0;
      bufferLen_ = n > 0 ? static_cast<size_t>(n) : 0;
      if (bufferLen_ == 0) {
        hadRow = !lineOut.empty();
        if (hadRow && !lineOut.empty() && lineOut.back() == '\r') lineOut.pop_back();
        return Error::None;
      }
    }
    const char c = static_cast<char>(buffer_[bufferPos_++]);
    ++logicalOffset;
    if (c == '\n') {
      hadRow = true;
      if (!lineOut.empty() && lineOut.back() == '\r') lineOut.pop_back();
      return Error::None;
    }
    if (lineOut.size() >= limits_.maxLineBytes) return Error::LineTooLong;
    lineOut.push_back(c);
  }
}

Error FileRowSource::seekToPage(int page, uint64_t& offsetOut) {
  const size_t targetRowIndex0 = static_cast<size_t>(page - 1) * static_cast<size_t>(pageSize_);
  const Cursor* anchor = bestCursor(targetRowIndex0);
  if (!anchor) return Error::SeekFailed;
  const size_t anchorRowIndex0 = anchor->rowIndex0;
  uint64_t offset = anchor->offset;
  if (!input_->seek(offset)) return Error::SeekFailed;
  bufferPos_ = 0;
  bufferLen_ = 0;

  const size_t skipRows = targetRowIndex0 - anchorRowIndex0;
  std::string ignored;
  for (size_t i = 0; i < skipRows; ++i) {
    bool hadRow = false;
    const Error e = readLine(ignored, hadRow, offset);
    if (e != Error::None) return e;
    if (!hadRow) return Error::ReadFailed;  // metadata and file contents diverged
  }
  rememberCursor(page, offset);
  offsetOut = offset;
  return Error::None;
}

PageResult FileRowSource::readPage(int requestedPage) {
  PageResult out;
  out.totalRows = rowCount_;
  out.totalPages = totalPages_;
  if (!isOpen()) {
    out.error = openError_;
    return out;
  }
  out.page = std::max(1, std::min(requestedPage, totalPages_));
  out.hasPrevious = out.page > 1;
  out.hasNext = out.page < totalPages_;
  if (rowCount_ == 0) return out;

  uint64_t offset = 0;
  out.error = seekToPage(out.page, offset);
  if (out.error != Error::None) return out;

  const size_t first = static_cast<size_t>(out.page - 1) * static_cast<size_t>(pageSize_);
  const size_t count = std::min(static_cast<size_t>(pageSize_), rowCount_ - first);
  out.rows.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Row row;
    row.index0 = first + i;
    bool hadRow = false;
    out.error = readLine(row.line, hadRow, offset);
    if (out.error != Error::None) {
      out.rows.clear();
      return out;
    }
    if (!hadRow) {
      out.error = Error::ReadFailed;
      out.rows.clear();
      return out;
    }
    out.rows.push_back(std::move(row));
  }
  if (out.hasNext) rememberCursor(out.page + 1, offset);
  return out;
}

}  // namespace M4FileRows

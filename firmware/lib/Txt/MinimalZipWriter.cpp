#include "MinimalZipWriter.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <miniz.h>

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool MinimalZipWriter::flushWriteBuffer() {
  if (writeBufPos_ == 0) return true;
  if (!writeBuf_) { writeBufPos_ = 0; return false; }
  const bool ok = outFile.write(writeBuf_, writeBufPos_) == static_cast<int>(writeBufPos_);
  writeBufPos_ = 0;
  return ok;
}

bool MinimalZipWriter::writeBytes(const void* data, size_t len) {
  // If buffer not available, fall back to direct writes
  if (!writeBuf_) {
    return outFile.write(static_cast<const uint8_t*>(data), len) == static_cast<int>(len);
  }
  const auto* src = static_cast<const uint8_t*>(data);
  while (len > 0) {
    const size_t space = WRITE_BUF_SIZE - writeBufPos_;
    const size_t chunk = std::min(len, space);
    memcpy(writeBuf_ + writeBufPos_, src, chunk);
    writeBufPos_ += chunk;
    src += chunk;
    len -= chunk;
    if (writeBufPos_ >= WRITE_BUF_SIZE) {
      if (!flushWriteBuffer()) return false;
    }
  }
  return true;
}

bool MinimalZipWriter::writeLE16(uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)};
  return writeBytes(b, 2);
}

bool MinimalZipWriter::writeLE32(uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                        static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>(v >> 24)};
  return writeBytes(b, 4);
}

// Writes a ZIP local file header (30 bytes + filename, no extra field).
// All multi-byte fields are little-endian.
bool MinimalZipWriter::writeLocalHeader(const char* name, uint16_t nameLen, uint32_t crc32, uint32_t size) {
  writeLE32(0x04034b50u);                    // Local file header signature
  writeLE16(20);                             // Version needed to extract (2.0)
  writeLE16(0);                              // General purpose bit flag
  writeLE16(0);                              // Compression method: STORE
  writeLE16(0);                              // Last mod file time
  writeLE16(0);                              // Last mod file date
  writeLE32(crc32);                          // CRC-32
  writeLE32(size);                           // Compressed size (== uncompressed for STORE)
  writeLE32(size);                           // Uncompressed size
  writeLE16(nameLen);                        // Filename length
  writeLE16(0);                              // Extra field length
  return writeBytes(name, nameLen);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool MinimalZipWriter::begin(const std::string& path) {
  entries.clear();
  writeBufPos_ = 0;
  if (!writeBuf_) {
    writeBuf_ = static_cast<uint8_t*>(malloc(WRITE_BUF_SIZE));
  }
  if (!SdMan.openFileForWrite("ZWR", path, outFile)) {
    Serial.printf("[%lu] [ZWR] Failed to open output ZIP: %s\n", millis(), path.c_str());
    return false;
  }
  return true;
}

bool MinimalZipWriter::addFile(const char* name, const uint8_t* data, uint32_t size) {
  const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data, size));
  flushWriteBuffer();  // flush before position query
  const uint32_t offset = static_cast<uint32_t>(outFile.position());
  const uint16_t nameLen = static_cast<uint16_t>(strlen(name));

  if (!writeLocalHeader(name, nameLen, crc, size)) {
    Serial.printf("[%lu] [ZWR] Failed to write header for: %s\n", millis(), name);
    return false;
  }
  if (size > 0 && !writeBytes(data, size)) {
    Serial.printf("[%lu] [ZWR] Failed to write data for: %s\n", millis(), name);
    return false;
  }

  entries.push_back({crc, size, offset});
  return true;
}

bool MinimalZipWriter::addFileFromFsFile(const char* name, FsFile& src, uint32_t size) {
  // First pass: compute CRC-32
  uint32_t crc = MZ_CRC32_INIT;
  uint8_t buf[512];
  uint32_t remaining = size;
  src.seek(0);
  while (remaining > 0) {
    const uint32_t chunk = std::min(remaining, static_cast<uint32_t>(sizeof(buf)));
    src.read(buf, chunk);
    crc = static_cast<uint32_t>(mz_crc32(crc, buf, chunk));
    remaining -= chunk;
  }

  flushWriteBuffer();  // flush before position query
  const uint32_t offset = static_cast<uint32_t>(outFile.position());
  const uint16_t nameLen = static_cast<uint16_t>(strlen(name));
  if (!writeLocalHeader(name, nameLen, crc, size)) {
    Serial.printf("[%lu] [ZWR] Failed to write header for: %s\n", millis(), name);
    return false;
  }

  // Second pass: write data
  remaining = size;
  src.seek(0);
  while (remaining > 0) {
    const uint32_t chunk = std::min(remaining, static_cast<uint32_t>(sizeof(buf)));
    src.read(buf, chunk);
    if (!writeBytes(buf, chunk)) {
      Serial.printf("[%lu] [ZWR] Failed to write data for: %s\n", millis(), name);
      return false;
    }
    remaining -= chunk;
  }

  entries.push_back({crc, size, offset});
  return true;
}

bool MinimalZipWriter::beginFileEntry(const char* name) {
  flushWriteBuffer();  // flush before position query
  streamHeaderOffset_ = static_cast<uint32_t>(outFile.position());
  streamSize_         = 0;
  streamCrc_          = MZ_CRC32_INIT;
  const uint16_t nameLen = static_cast<uint16_t>(strlen(name));
  return writeLocalHeader(name, nameLen, 0, 0);  // size/CRC are placeholder, patched in endFileEntry()
}

bool MinimalZipWriter::writeFileChunk(const uint8_t* data, uint32_t size) {
  if (size == 0) return true;
  streamCrc_ = static_cast<uint32_t>(mz_crc32(streamCrc_, data, size));
  streamSize_ += size;
  return writeBytes(data, size);
}

bool MinimalZipWriter::endFileEntry() {
  flushWriteBuffer();  // flush before position query and seek
  const uint32_t dataEnd = static_cast<uint32_t>(outFile.position());
  outFile.seek(streamHeaderOffset_ + 14);
  writeLE32(streamCrc_);
  writeLE32(streamSize_);
  writeLE32(streamSize_);
  flushWriteBuffer();  // flush patched header before seeking back
  outFile.seek(dataEnd);
  entries.push_back({streamCrc_, streamSize_, streamHeaderOffset_});
  return true;
}

bool MinimalZipWriter::end() {
  flushWriteBuffer();

  // Pre-read all filenames in a single forward pass through local headers.
  // Entries are stored in file order, so seeks are mostly forward (fast on SD).
  // This avoids interleaved read-seek-write-seek per entry when writing the
  // central directory, saving ~1 seek per entry (~7s for 1400 chapters).
  struct NameInfo { uint16_t len; char name[48]; };
  const size_t entryCount = entries.size();
  auto* names = new (std::nothrow) NameInfo[entryCount];
  if (names) {
    for (size_t i = 0; i < entryCount; i++) {
      outFile.seek(entries[i].localHeaderOffset + 26);
      uint8_t lenBytes[2];
      outFile.read(lenBytes, 2);
      names[i].len = static_cast<uint16_t>(lenBytes[0] | (lenBytes[1] << 8));
      if (names[i].len >= sizeof(names[i].name))
        names[i].len = sizeof(names[i].name) - 1;
      outFile.seek(entries[i].localHeaderOffset + 30);
      outFile.read(reinterpret_cast<uint8_t*>(names[i].name), names[i].len);
    }
  }

  // Seek to end of data, then write central directory sequentially (no more seeks)
  if (entryCount > 0) {
    const auto& last = entries.back();
    // End of last entry = local header offset + 30 + filename length + data size
    outFile.seek(last.localHeaderOffset + 26);
    uint8_t lb[2]; outFile.read(lb, 2);
    const uint16_t lastNameLen = static_cast<uint16_t>(lb[0] | (lb[1] << 8));
    outFile.seek(last.localHeaderOffset + 30 + lastNameLen + last.size);
  }

  const uint32_t centralDirOffset = static_cast<uint32_t>(outFile.position());

  for (size_t i = 0; i < entryCount; i++) {
    const auto& e = entries[i];
    uint16_t nameLen;
    const char* nameBuf;

    if (names) {
      nameLen = names[i].len;
      nameBuf = names[i].name;
    } else {
      // Fallback: read from file (slower, but shouldn't happen)
      static char fallbackName[48];
      const uint32_t writePos = static_cast<uint32_t>(outFile.position());
      outFile.seek(e.localHeaderOffset + 26);
      uint8_t lenBytes[2];
      outFile.read(lenBytes, 2);
      nameLen = static_cast<uint16_t>(lenBytes[0] | (lenBytes[1] << 8));
      if (nameLen >= sizeof(fallbackName)) nameLen = sizeof(fallbackName) - 1;
      outFile.seek(e.localHeaderOffset + 30);
      outFile.read(reinterpret_cast<uint8_t*>(fallbackName), nameLen);
      outFile.seek(writePos);
      nameBuf = fallbackName;
    }

    writeLE32(0x02014b50u);  // Central directory signature
    writeLE16(20);           // Version made by (2.0 / MS-DOS)
    writeLE16(20);           // Version needed to extract (2.0)
    writeLE16(0);            // General purpose bit flag
    writeLE16(0);            // Compression method: STORE
    writeLE16(0);            // Last mod file time
    writeLE16(0);            // Last mod file date
    writeLE32(e.crc32);      // CRC-32
    writeLE32(e.size);       // Compressed size
    writeLE32(e.size);       // Uncompressed size
    writeLE16(nameLen);      // Filename length
    writeLE16(0);            // Extra field length
    writeLE16(0);            // File comment length
    writeLE16(0);            // Disk number start
    writeLE16(0);            // Internal file attributes
    writeLE32(0);            // External file attributes
    writeLE32(e.localHeaderOffset);  // Offset of local header
    writeBytes(nameBuf, nameLen);
  }

  delete[] names;

  flushWriteBuffer();
  const uint32_t centralDirEnd = static_cast<uint32_t>(outFile.position());
  const uint32_t centralDirSize = centralDirEnd - centralDirOffset;
  const auto count16 = static_cast<uint16_t>(entryCount);

  // Write end of central directory record (EOCD)
  writeLE32(0x06054b50u);    // EOCD signature
  writeLE16(0);              // Disk number
  writeLE16(0);              // Disk with start of central directory
  writeLE16(count16);        // Entries on this disk
  writeLE16(count16);        // Total entries
  writeLE32(centralDirSize); // Size of central directory
  writeLE32(centralDirOffset); // Offset of central directory
  writeLE16(0);              // Comment length

  flushWriteBuffer();
  free(writeBuf_);
  writeBuf_ = nullptr;
  writeBufPos_ = 0;
  outFile.sync();
  outFile.close();
  Serial.printf("[%lu] [ZWR] ZIP finalised: %d entries\n", millis(), static_cast<int>(entryCount));
  return true;
}

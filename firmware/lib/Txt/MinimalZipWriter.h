#pragma once

#include <SDCardManager.h>

#include <cstring>
#include <string>
#include <vector>

// Writes a ZIP file with STORE (no compression) entries directly to SdFat FsFile.
// Suitable for generating EPUB files from TXT content on ESP32.
// EPUB spec requires 'mimetype' to be added first via addFile().
class MinimalZipWriter {
 public:
  // Compact entry info – filenames are re-read from local headers in end()
  // to avoid storing thousands of name strings for large books (2000+ chapters).
  struct EntryInfo {
    uint32_t crc32;
    uint32_t size;
    uint32_t localHeaderOffset;
  };

 private:
  FsFile outFile;
  std::vector<EntryInfo> entries;

  // Write buffer: accumulates small writes and flushes in larger chunks
  // to reduce SD card write syscall overhead (~50× fewer calls for typical EPUB).
  // Heap-allocated to avoid stack overflow on ESP32 loopTask (8KB stack).
  static constexpr size_t WRITE_BUF_SIZE = 2048;
  uint8_t* writeBuf_ = nullptr;
  size_t writeBufPos_ = 0;

  bool flushWriteBuffer();
  bool writeBytes(const void* data, size_t len);
  bool writeLE16(uint16_t v);
  bool writeLE32(uint32_t v);
  bool writeLocalHeader(const char* name, uint16_t nameLen, uint32_t crc32, uint32_t size);

  // State for the in-progress streaming entry (used by begin/writeChunk/endFileEntry)
  uint32_t streamHeaderOffset_ = 0;
  uint32_t streamSize_         = 0;
  uint32_t streamCrc_          = 0;

 public:
  // Open the output ZIP file for writing. Must be called before any addFile().
  bool begin(const std::string& path);

  // Add a file from a memory buffer (computes CRC internally).
  bool addFile(const char* name, const uint8_t* data, uint32_t size);

  // Add a file by reading from an already-opened FsFile.
  // Reads from the beginning of src; 'size' must equal src.size().
  bool addFileFromFsFile(const char* name, FsFile& src, uint32_t size);

  // Streaming API – write large files without holding the entire content in memory.
  // Usage: beginFileEntry() → writeFileChunk() × N → endFileEntry()
  // The local-header size/CRC fields are back-patched when endFileEntry() is called.
  bool beginFileEntry(const char* name);
  bool writeFileChunk(const uint8_t* data, uint32_t size);
  bool endFileEntry();

  // Finalise the archive: write central directory + EOCD, then close.
  bool end();
};

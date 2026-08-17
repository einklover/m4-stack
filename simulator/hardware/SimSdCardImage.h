#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace m4sim {

// Sector-addressable backing store shared by the deterministic SDMMC model and
// host-side QEMU preparation tools.  It deliberately models a raw card image,
// not FAT: the guest owns partition/filesystem semantics.
class SimSdCardImage {
 public:
  static constexpr std::size_t kBlockBytes = 512;
  using Block = std::array<uint8_t, kBlockBytes>;

  bool create(std::size_t blocks, uint8_t fill = 0x00) {
    if (blocks == 0 || blocks > maxBlocks()) return false;
    bytes_.assign(blocks * kBlockBytes, fill);
    writable_ = true;
    sourcePath_.clear();
    dirty_ = false;
    return true;
  }

  bool load(const std::string& path, bool writable = true) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto end = in.tellg();
    if (end <= 0) return false;
    const auto size = static_cast<std::size_t>(end);
    if (size % kBlockBytes != 0 || size / kBlockBytes > maxBlocks()) return false;
    std::vector<uint8_t> next(size);
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(next.data()), static_cast<std::streamsize>(size))) {
      return false;
    }
    bytes_.swap(next);
    writable_ = writable;
    sourcePath_ = path;
    dirty_ = false;
    return true;
  }

  bool save(const std::string& path = std::string()) {
    if (bytes_.empty()) return false;
    const std::string& target = path.empty() ? sourcePath_ : path;
    if (target.empty()) return false;
    if (!writable_ && path.empty()) return false;
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes_.data()),
              static_cast<std::streamsize>(bytes_.size()));
    if (!out) return false;
    if (!path.empty()) sourcePath_ = path;
    dirty_ = false;
    return true;
  }

  bool readBlock(uint32_t lba, Block& out) const {
    if (lba >= blocks()) return false;
    const std::size_t off = static_cast<std::size_t>(lba) * kBlockBytes;
    for (std::size_t i = 0; i < kBlockBytes; ++i) out[i] = bytes_[off + i];
    return true;
  }

  bool writeBlock(uint32_t lba, const Block& in) {
    if (!writable_ || lba >= blocks()) return false;
    const std::size_t off = static_cast<std::size_t>(lba) * kBlockBytes;
    for (std::size_t i = 0; i < kBlockBytes; ++i) bytes_[off + i] = in[i];
    dirty_ = true;
    return true;
  }

  void clear() {
    bytes_.clear();
    sourcePath_.clear();
    writable_ = true;
    dirty_ = false;
  }

  std::size_t blocks() const { return bytes_.size() / kBlockBytes; }
  std::size_t bytes() const { return bytes_.size(); }
  bool empty() const { return bytes_.empty(); }
  bool writable() const { return writable_; }
  bool dirty() const { return dirty_; }
  const std::string& sourcePath() const { return sourcePath_; }

 private:
  // Protect the deterministic simulator from accidental multi-gigabyte test
  // allocations while remaining well above typical reader-card fixtures.
  static constexpr std::size_t maxBlocks() {
    return (512u * 1024u * 1024u) / kBlockBytes;  // 512 MiB host fixture cap
  }

  std::vector<uint8_t> bytes_;
  bool writable_ = true;
  bool dirty_ = false;
  std::string sourcePath_;
};

}  // namespace m4sim

#include "util/M4ProviderCoverCache.h"

#include <Bitmap.h>
#include <JpegToBmpConverter.h>
#include <PNGdec.h>
#include <SDCardManager.h>
#include <CoverDither.h>

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4Psram.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>

namespace {

class FileSink final : public M4xJsonStream::Sink {
 public:
  explicit FileSink(FsFile& file) : file_(file) {}

  bool write(const uint8_t* data, size_t len) override {
    if (len == 0) return true;
    if (failed_ || !data) return false;
    if (file_.write(data, len) != len) {
      failed_ = true;
      return false;
    }
    return true;
  }

  bool failed() const { return failed_; }

 private:
  FsFile& file_;
  bool failed_ = false;
};

struct PngBmpContext {
  PNG* decoder = nullptr;
  FsFile* output = nullptr;
  uint8_t* targetImage = nullptr;
  uint16_t* sourceRow = nullptr;
  int sourceWidth = 0;
  int sourceHeight = 0;
  int targetWidth = 0;
  int targetHeight = 0;
};

void writeLe16(FsFile& file, uint16_t value) {
  file.write(static_cast<uint8_t>(value & 0xffu));
  file.write(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void writeLe32(FsFile& file, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    file.write(static_cast<uint8_t>(value & 0xffu));
    value >>= 8;
  }
}

bool writeBmpHeader1bit(FsFile& file, int width, int height) {
  const int rowBytes = (width + 31) / 32 * 4;
  const int imageSize = rowBytes * height;
  const uint32_t fileSize = 62u + static_cast<uint32_t>(imageSize);
  if (file.write('B') != 1 || file.write('M') != 1) return false;
  writeLe32(file, fileSize);
  writeLe32(file, 0);
  writeLe32(file, 62);
  writeLe32(file, 40);
  writeLe32(file, static_cast<uint32_t>(width));
  writeLe32(file, static_cast<uint32_t>(-height));
  writeLe16(file, 1);
  writeLe16(file, 1);
  writeLe32(file, 0);
  writeLe32(file, static_cast<uint32_t>(imageSize));
  writeLe32(file, 2835);
  writeLe32(file, 2835);
  writeLe32(file, 2);
  writeLe32(file, 2);
  uint8_t palette[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (uint8_t v : palette) {
    if (file.write(v) != 1) return false;
  }
  return true;
}

bool writePngBmpHeader(FsFile& file, int width, int height) {
  const int rowBytes = (width + 3) & ~3;
  const uint32_t paletteBytes = 256u * 4u;
  const uint32_t imageBytes = static_cast<uint32_t>(rowBytes) * static_cast<uint32_t>(height);
  const uint32_t fileBytes = 14u + 40u + paletteBytes + imageBytes;
  if (file.write('B') != 1 || file.write('M') != 1) return false;
  writeLe32(file, fileBytes);
  writeLe32(file, 0);
  writeLe32(file, 14u + 40u + paletteBytes);
  writeLe32(file, 40);
  writeLe32(file, static_cast<uint32_t>(width));
  writeLe32(file, static_cast<uint32_t>(-height));
  writeLe16(file, 1);
  writeLe16(file, 8);
  writeLe32(file, 0);
  writeLe32(file, imageBytes);
  writeLe32(file, 2835);
  writeLe32(file, 2835);
  writeLe32(file, 256);
  writeLe32(file, 256);
  for (int i = 0; i < 256; ++i) {
    if (file.write(static_cast<uint8_t>(i)) != 1 || file.write(static_cast<uint8_t>(i)) != 1 ||
        file.write(static_cast<uint8_t>(i)) != 1 || file.write(static_cast<uint8_t>(0)) != 1) {
      return false;
    }
  }
  return true;
}

void* pngOpen(const char* path, int32_t* size) {
  auto* file = new FsFile();
  if (!SdMan.openFileForRead("M4CoverPNG", path, *file)) {
    delete file;
    return nullptr;
  }
  if (size) *size = static_cast<int32_t>(file->size());
  return file;
}

void pngClose(void* handle) {
  auto* file = static_cast<FsFile*>(handle);
  if (!file) return;
  file->close();
  delete file;
}

int32_t pngRead(PNGFILE* handle, uint8_t* buffer, int32_t length) {
  if (!handle || !handle->fHandle || !buffer || length <= 0) return 0;
  auto* file = static_cast<FsFile*>(handle->fHandle);
  const int n = file->read(buffer, static_cast<size_t>(length));
  return n > 0 ? n : 0;
}

int32_t pngSeek(PNGFILE* handle, int32_t position) {
  if (!handle || !handle->fHandle || position < 0) return 0;
  auto* file = static_cast<FsFile*>(handle->fHandle);
  return file->seek(static_cast<uint32_t>(position)) ? position : 0;
}

uint8_t luminance565(uint16_t pixel) {
  const uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
  const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
  const uint8_t b = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
  return static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
}

int pngDraw(PNGDRAW* draw) {
  if (!draw || !draw->pUser) return 0;
  auto* ctx = static_cast<PngBmpContext*>(draw->pUser);
  if (!ctx->decoder || !ctx->targetImage || !ctx->sourceRow ||
      draw->iWidth != ctx->sourceWidth || draw->y < 0 || draw->y >= ctx->sourceHeight) {
    return 0;
  }
  ctx->decoder->getLineAsRGB565(draw, ctx->sourceRow, PNG_RGB565_LITTLE_ENDIAN, 0x00ffffffu);
  const int firstTargetY = draw->y * ctx->targetHeight / ctx->sourceHeight;
  const int nextTargetY = (draw->y + 1) * ctx->targetHeight / ctx->sourceHeight;
  for (int targetY = firstTargetY; targetY < nextTargetY; ++targetY) {
    for (int x = 0; x < ctx->targetWidth; ++x) {
      const int sourceX = x * ctx->sourceWidth / ctx->targetWidth;
      ctx->targetImage[static_cast<size_t>(targetY) * static_cast<size_t>(ctx->targetWidth) + x] =
          luminance565(ctx->sourceRow[sourceX]);
    }
  }
  return 1;
}

bool pngFileToBmpStream(const std::string& sourcePath, const std::string& targetPath,
                        int width, int height, bool oneBit) {
  if (width <= 0 || height <= 0 || width > 2048 || height > 3072 ||
      !M4NativeProviderHeavyGate::heapHealthy(0x420)) {
    return false;
  }
  auto decoder = M4Psram::makeUnique<PNG>();
  if (!decoder) return false;
  if (decoder->open(sourcePath.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw) != PNG_SUCCESS) {
    return false;
  }

  FsFile output;
  if (!SdMan.openFileForWrite("M4CoverPNG", targetPath.c_str(), output)) {
    decoder->close();
    return false;
  }
  const int sourceWidth = decoder->getWidth();
  const int sourceHeight = decoder->getHeight();
  if (sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > 2048 || sourceHeight > 3072) {
    output.close();
    decoder->close();
    return false;
  }

  auto* targetImage = static_cast<uint8_t*>(M4Psram::mallocPrefer(static_cast<size_t>(width) * height));
  auto* sourceRow = static_cast<uint16_t*>(M4Psram::mallocPrefer(
      static_cast<size_t>(sourceWidth) * sizeof(uint16_t)));
  if (!targetImage || !sourceRow) {
    M4Psram::freePrefer(targetImage);
    M4Psram::freePrefer(sourceRow);
    output.close();
    decoder->close();
    return false;
  }

  PngBmpContext ctx{decoder.get(), &output, targetImage, sourceRow, sourceWidth, sourceHeight,
                    width, height};
  const bool ok = decoder->decode(&ctx, PNG_CHECK_CRC) == PNG_SUCCESS;
  bool writeOk = ok;
  uint8_t* ditherWork = nullptr;
  uint8_t* ditherSmooth = nullptr;
  if (writeOk && oneBit) {
    ditherWork = static_cast<uint8_t*>(M4Psram::mallocPrefer(static_cast<size_t>(width) * height));
    ditherSmooth = static_cast<uint8_t*>(M4Psram::mallocPrefer(static_cast<size_t>(width) * height));
    writeOk = ditherWork && ditherSmooth && M4CoverDither::prepare(targetImage, ditherWork, ditherSmooth, width, height);
  }
  if (writeOk) {
    if (oneBit) {
      writeOk = writeBmpHeader1bit(output, width, height);
      const int rowBytes = (width + 31) / 32 * 4;
      uint8_t* row = static_cast<uint8_t*>(M4Psram::mallocPrefer(static_cast<size_t>(rowBytes)));
      for (int y = 0; row && y < height && writeOk; ++y) {
        std::memset(row, 0, static_cast<size_t>(rowBytes));
        for (int x = 0; x < width; ++x)
          if (M4CoverDither::pixelToBit(targetImage, ditherWork, width, x, y))
            row[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
        writeOk = output.write(row, static_cast<size_t>(rowBytes)) == rowBytes;
      }
      if (!row) writeOk = false;
      M4Psram::freePrefer(row);
    } else {
      writeOk = writePngBmpHeader(output, width, height);
      const int rowBytes = (width + 3) & ~3;
      for (int y = 0; y < height && writeOk; ++y) {
        if (output.write(targetImage + static_cast<size_t>(y) * width, static_cast<size_t>(width)) != width)
          writeOk = false;
        uint8_t padding[3] = {0, 0, 0};
        const int paddingBytes = rowBytes - width;
        if (paddingBytes && output.write(padding, static_cast<size_t>(paddingBytes)) != paddingBytes) writeOk = false;
      }
    }
  }
  M4Psram::freePrefer(ditherWork);
  M4Psram::freePrefer(ditherSmooth);
  M4Psram::freePrefer(targetImage);
  M4Psram::freePrefer(sourceRow);
  output.close();
  decoder->close();
  return writeOk;
}

bool bmpFileTo1BitBmpWithSize(const std::string& sourcePath, const std::string& targetPath,
                                   int dstW, int dstH) {
  if (dstW <= 0 || dstH <= 0 || dstW > 2048 || dstH > 3072 ||
      !M4NativeProviderHeavyGate::heapHealthy(0x420)) {
    return false;
  }
  FsFile src;
  if (!SdMan.openFileForRead("M4Cover", sourcePath.c_str(), src)) return false;

  auto readU16 = [](FsFile& f, uint16_t& out) -> bool {
    int c0 = f.read();
    int c1 = f.read();
    if (c0 < 0 || c1 < 0) return false;
    out = static_cast<uint16_t>(static_cast<uint8_t>(c0) | (static_cast<uint16_t>(static_cast<uint8_t>(c1)) << 8));
    return true;
  };
  auto readU32 = [](FsFile& f, uint32_t& out) -> bool {
    int c0 = f.read();
    int c1 = f.read();
    int c2 = f.read();
    int c3 = f.read();
    if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return false;
    out = static_cast<uint32_t>(static_cast<uint8_t>(c0)) |
          (static_cast<uint32_t>(static_cast<uint8_t>(c1)) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(c2)) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(c3)) << 24);
    return true;
  };
  auto readI32 = [&](FsFile& f, int32_t& out) -> bool {
    uint32_t u;
    if (!readU32(f, u)) return false;
    out = static_cast<int32_t>(u);
    return true;
  };

  if (!src.seek(0)) { src.close(); return false; }
  uint16_t bfType;
  if (!readU16(src, bfType) || bfType != 0x4D42) { src.close(); return false; }
  uint32_t bfSizeDummy;
  if (!readU32(src, bfSizeDummy)) { src.close(); return false; }
  uint32_t reserved;
  if (!readU32(src, reserved)) { src.close(); return false; }
  uint32_t bfOffBits;
  if (!readU32(src, bfOffBits)) { src.close(); return false; }
  uint32_t biSize;
  if (!readU32(src, biSize) || biSize < 40) { src.close(); return false; }
  int32_t biWidth;
  if (!readI32(src, biWidth)) { src.close(); return false; }
  int32_t biHeightRaw;
  if (!readI32(src, biHeightRaw)) { src.close(); return false; }
  if (biWidth == INT32_MIN || biHeightRaw == INT32_MIN) { src.close(); return false; }
  const bool topDown = biHeightRaw < 0;
  const int srcW = static_cast<int>(biWidth);
  const int srcH = topDown ? -biHeightRaw : biHeightRaw;
  uint16_t planes;
  if (!readU16(src, planes) || planes != 1) { src.close(); return false; }
  uint16_t bpp;
  if (!readU16(src, bpp)) { src.close(); return false; }
  const bool validBpp = (bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32);
  if (!validBpp) { src.close(); return false; }
  uint32_t comp;
  if (!readU32(src, comp)) { src.close(); return false; }
  if (!(comp == 0 || (bpp == 32 && comp == 3))) { src.close(); return false; }
  uint32_t biSizeImageDummy;
  if (!readU32(src, biSizeImageDummy)) { src.close(); return false; }
  uint32_t xPels, yPels;
  if (!readU32(src, xPels)) { src.close(); return false; }
  if (!readU32(src, yPels)) { src.close(); return false; }
  uint32_t colorsUsed;
  if (!readU32(src, colorsUsed) || colorsUsed > 256) { src.close(); return false; }
  uint32_t colorsImportant;
  if (!readU32(src, colorsImportant)) { src.close(); return false; }

  if (srcW <= 0 || srcH <= 0 || srcW > 2048 || srcH > 3072) { src.close(); return false; }
  const int srcRowBytes = (srcW * bpp + 31) / 32 * 4;
  if (srcRowBytes <= 0 || srcRowBytes > 8192) { src.close(); return false; }

  // Palette
  uint8_t paletteLum[256];
  for (int i = 0; i < 256; ++i) paletteLum[i] = static_cast<uint8_t>(i);
  uint32_t palEntries = 0;
  if (bpp <= 8) {
    palEntries = colorsUsed ? colorsUsed : (1u << bpp);
    if (palEntries > 256) palEntries = 256;
  }
  if (palEntries > 0) {
    const uint32_t paletteOffset = 14u + biSize;
    if (!src.seek(paletteOffset)) { src.close(); return false; }
    for (uint32_t i = 0; i < palEntries; ++i) {
      uint8_t rgb[4];
      if (src.read(rgb, 4) != 4) { src.close(); return false; }
      paletteLum[i] = static_cast<uint8_t>((77u * rgb[2] + 150u * rgb[1] + 29u * rgb[0]) >> 8);
    }
  }
  if (!src.seek(bfOffBits)) { src.close(); return false; }

  const size_t srcPixels = static_cast<size_t>(srcW) * static_cast<size_t>(srcH);
  uint8_t* srcGray = static_cast<uint8_t*>(M4Psram::mallocPrefer(srcPixels));
  if (!srcGray) { src.close(); return false; }
  uint8_t* rawRow = static_cast<uint8_t*>(M4Psram::mallocPrefer(static_cast<size_t>(srcRowBytes)));
  if (!rawRow) { M4Psram::freePrefer(srcGray); src.close(); return false; }

  bool loadOk = true;
  for (int i = 0; i < srcH; ++i) {
    if (src.read(rawRow, static_cast<size_t>(srcRowBytes)) != srcRowBytes) { loadOk = false; break; }
    const int y = topDown ? i : (srcH - 1 - i);
    uint8_t* dst = srcGray + static_cast<size_t>(y) * static_cast<size_t>(srcW);
    switch (bpp) {
      case 1: {
        for (int x = 0; x < srcW; ++x) {
          const uint8_t idx = (rawRow[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
          dst[x] = paletteLum[idx];
        }
        break;
      }
      case 2: {
        for (int x = 0; x < srcW; ++x) {
          const uint8_t idx = (rawRow[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03;
          dst[x] = paletteLum[idx];
        }
        break;
      }
      case 4: {
        for (int x = 0; x < srcW; ++x) {
          const uint8_t v = rawRow[x >> 1];
          const uint8_t idx = (x & 1) ? (v & 0x0F) : (v >> 4);
          dst[x] = paletteLum[idx];
        }
        break;
      }
      case 8: {
        for (int x = 0; x < srcW; ++x) dst[x] = paletteLum[rawRow[x]];
        break;
      }
      case 24: {
        for (int x = 0; x < srcW; ++x) {
          const uint8_t b = rawRow[x * 3 + 0];
          const uint8_t g = rawRow[x * 3 + 1];
          const uint8_t r = rawRow[x * 3 + 2];
          dst[x] = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
        }
        break;
      }
      case 32: {
        for (int x = 0; x < srcW; ++x) {
          const uint8_t b = rawRow[x * 4 + 0];
          const uint8_t g = rawRow[x * 4 + 1];
          const uint8_t r = rawRow[x * 4 + 2];
          dst[x] = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
        }
        break;
      }
      default: loadOk = false; break;
    }
    if (!loadOk) break;
  }
  M4Psram::freePrefer(rawRow);
  src.close();
  if (!loadOk) { M4Psram::freePrefer(srcGray); return false; }

  // Compute aspect-fill crop
  int srcCropW = srcW;
  int srcCropH = srcH;
  int srcX0 = 0;
  int srcY0 = 0;
  if (static_cast<uint64_t>(srcW) * static_cast<uint64_t>(dstH) > static_cast<uint64_t>(srcH) * static_cast<uint64_t>(dstW)) {
    srcCropW = static_cast<int>(static_cast<uint64_t>(srcH) * static_cast<uint64_t>(dstW) / static_cast<uint64_t>(dstH));
    if (srcCropW < 1) srcCropW = 1;
    if (srcCropW > srcW) srcCropW = srcW;
    srcX0 = (srcW - srcCropW) / 2;
  } else {
    srcCropH = static_cast<int>(static_cast<uint64_t>(srcW) * static_cast<uint64_t>(dstH) / static_cast<uint64_t>(dstW));
    if (srcCropH < 1) srcCropH = 1;
    if (srcCropH > srcH) srcCropH = srcH;
    srcY0 = (srcH - srcCropH) / 2;
  }

  FsFile out;
  if (!SdMan.openFileForWrite("M4Cover", targetPath.c_str(), out)) {
    M4Psram::freePrefer(srcGray);
    return false;
  }
  if (!writeBmpHeader1bit(out, dstW, dstH)) {
    out.close();
    M4Psram::freePrefer(srcGray);
    return false;
  }
  const int rowBytesDst = (dstW + 31) / 32 * 4;
  uint8_t* targetRow = static_cast<uint8_t*>(M4Psram::mallocPrefer(static_cast<size_t>(rowBytesDst)));
  if (!targetRow) {
    out.close();
    M4Psram::freePrefer(srcGray);
    return false;
  }
  const size_t targetPixels = static_cast<size_t>(dstW) * static_cast<size_t>(dstH);
  auto* targetGray = static_cast<uint8_t*>(M4Psram::mallocPrefer(targetPixels));
  auto* ditherWork = static_cast<uint8_t*>(M4Psram::mallocPrefer(targetPixels));
  auto* ditherSmooth = static_cast<uint8_t*>(M4Psram::mallocPrefer(targetPixels));
  if (!targetGray || !ditherWork || !ditherSmooth) {
    M4Psram::freePrefer(targetGray); M4Psram::freePrefer(ditherWork); M4Psram::freePrefer(ditherSmooth);
    M4Psram::freePrefer(targetRow); out.close(); return false;
  }
  bool ok = true;
  for (int ty = 0; ty < dstH; ++ty) {
    std::memset(targetRow, 0, static_cast<size_t>(rowBytesDst));
    const int srcYStart = srcY0 + ty * srcCropH / dstH;
    int srcYEnd = srcY0 + (ty + 1) * srcCropH / dstH;
    if (srcYEnd <= srcYStart) srcYEnd = srcYStart + 1;
    if (srcYEnd > srcY0 + srcCropH) srcYEnd = srcY0 + srcCropH;
    for (int tx = 0; tx < dstW; ++tx) {
      const int srcXStart = srcX0 + tx * srcCropW / dstW;
      int srcXEnd = srcX0 + (tx + 1) * srcCropW / dstW;
      if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
      if (srcXEnd > srcX0 + srcCropW) srcXEnd = srcX0 + srcCropW;
      int sum = 0;
      int count = 0;
      for (int sy = srcYStart; sy < srcYEnd; ++sy) {
        const uint8_t* row = srcGray + static_cast<size_t>(sy) * static_cast<size_t>(srcW);
        for (int sx = srcXStart; sx < srcXEnd; ++sx) {
          sum += row[sx];
          ++count;
        }
      }
      targetGray[ty * dstW + tx] = count ? static_cast<uint8_t>(sum / count) : 0;
    }
  }
  if (ok && M4CoverDither::prepare(targetGray, ditherWork, ditherSmooth, dstW, dstH)) {
    for (int y = 0; y < dstH && ok; ++y) {
      std::memset(targetRow, 0, static_cast<size_t>(rowBytesDst));
      for (int x = 0; x < dstW; ++x) {
        if (M4CoverDither::pixelToBit(targetGray, ditherWork, dstW, x, y))
          targetRow[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
      }
      if (out.write(targetRow, static_cast<size_t>(rowBytesDst)) != rowBytesDst) ok = false;
    }
  } else {
    ok = false;
  }
  M4Psram::freePrefer(targetGray);
  M4Psram::freePrefer(ditherWork);
  M4Psram::freePrefer(ditherSmooth);
  M4Psram::freePrefer(targetRow);
  M4Psram::freePrefer(srcGray);
  out.close();
  return ok;
}

bool copyFile(const std::string& sourcePath, const std::string& targetPath) {
  FsFile source;
  FsFile target;
  if (!SdMan.openFileForRead("M4Cover", sourcePath.c_str(), source) ||
      !SdMan.openFileForWrite("M4Cover", targetPath.c_str(), target)) {
    source.close();
    target.close();
    return false;
  }
  uint8_t buffer[4096];
  bool ok = true;
  while (source.available()) {
    const int n = source.read(buffer, sizeof(buffer));
    if (n <= 0 || target.write(buffer, static_cast<size_t>(n)) != n) {
      ok = false;
      break;
    }
  }
  source.close();
  target.close();
  return ok;
}

M4ProviderCoverCache::ImageFormat sniffFile(FsFile& file) {
  uint8_t magic[12] = {};
  if (!file.seek(0) || file.read(magic, sizeof(magic)) <= 0) return M4ProviderCoverCache::ImageFormat::Unknown;
  return M4ProviderCoverCache::detectImageFormat(magic, sizeof(magic));
}

bool convertCoverFile(const std::string& source, const std::string& target, int width, int height, bool oneBit) {
  using M4ProviderCoverCache::ImageFormat;
  FsFile sniff;
  if (!SdMan.openFileForRead("M4Cover", source.c_str(), sniff)) return false;
  const auto format = sniffFile(sniff);
  sniff.close();
  if (format == ImageFormat::Bmp) {
    if (!oneBit) {
      FsFile input;
      if (!SdMan.openFileForRead("M4Cover", source.c_str(), input)) return false;
      Bitmap bitmap(input);
      const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
      input.close();
      return valid && copyFile(source, target);
    }
    // One-bit exact: scale the BMP (including 2-bit Fengyan 171x254) to WxH via dithered 1-bit.
    return bmpFileTo1BitBmpWithSize(source, target, width, height);
  }
  if (format == ImageFormat::Png) return pngFileToBmpStream(source, target, width, height, oneBit);
  if (format != ImageFormat::Jpeg) return false;
  FsFile input;
  FsFile output;
  if (!SdMan.openFileForRead("M4Cover", source.c_str(), input) ||
      !SdMan.openFileForWrite("M4Cover", target.c_str(), output)) {
    input.close();
    output.close();
    return false;
  }
  const bool ok = oneBit ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, width, height)
                         : JpegToBmpConverter::jpegFileToBmpStreamWithSize(input, output, width, height);
  input.close();
  output.close();
  return ok;
}

}  // namespace

namespace M4ProviderCoverCache {

Result acquireProviderCover(const Request& request) {
  Backend backend;
  backend.exists = [](const std::string& path) { return SdMan.exists(path.c_str()); };
  backend.makeDirs = [](const std::string& dir) {
    SdMan.mkdir("/.crosspoint");
    SdMan.mkdir("/.crosspoint/provider_covers");
    return SdMan.exists(dir.c_str()) || SdMan.mkdir(dir.c_str());
  };
  backend.fetchCancellable = [](const std::string& url, const std::string& path, size_t maxBytes,
                                const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) return false;
    FsFile output;
    if (!SdMan.openFileForWrite("M4CoverDownload", path.c_str(), output)) return false;
    FileSink sink(output);
    M4NativeProviderHttp::Request request;
    request.url = url;
    request.maxBytes = maxBytes;
    request.timeoutMs = 20000;
    request.followRedirects = true;
    const auto net = M4NativeProviderHttp::requestToSink(request, sink, {}, cancelled);
    output.close();
    const bool ok = net.ok && !sink.failed() && !(cancelled && cancelled());
    if (!ok) SdMan.remove(path.c_str());
    return ok;
  };
  backend.convert = [](const std::string& source, const std::string& target, int width, int height) {
    return convertCoverFile(source, target, width, height, false);
  };
  backend.remove = [](const std::string& path) { SdMan.remove(path.c_str()); };
  return acquire(request, backend);
}

bool ensureSizedCoverFromSource(const std::string& coverBmpPath, int width, int height,
                                const std::function<bool()>& cancelled) {
  Backend backend;
  backend.exists = [](const std::string& path) { return SdMan.exists(path.c_str()); };
  backend.convert = [](const std::string& source, const std::string& target, int w, int h) {
    return convertCoverFile(source, target, w, h, true);
  };
  backend.remove = [](const std::string& path) { SdMan.remove(path.c_str()); };
  const auto result = ensureSizedCoverFromSource(coverBmpPath, width, height, backend, cancelled);
  return !result.thumbPath.empty();
}

}  // namespace M4ProviderCoverCache

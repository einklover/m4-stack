#include "util/M4ProviderCoverCache.h"

#include <Bitmap.h>
#include <JpegToBmpConverter.h>
#include <PNGdec.h>
#include <SDCardManager.h>

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4Psram.h"

#include <algorithm>
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
  uint8_t* targetRow = nullptr;
  uint16_t* sourceRow = nullptr;
  int sourceWidth = 0;
  int sourceHeight = 0;
  int targetWidth = 0;
  int targetHeight = 0;
  int rowBytes = 0;
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
  if (!ctx->decoder || !ctx->output || !ctx->targetRow || !ctx->sourceRow ||
      draw->iWidth != ctx->sourceWidth || draw->y < 0 || draw->y >= ctx->sourceHeight) {
    return 0;
  }
  ctx->decoder->getLineAsRGB565(draw, ctx->sourceRow, PNG_RGB565_LITTLE_ENDIAN, 0x00ffffffu);
  for (int x = 0; x < ctx->targetWidth; ++x) {
    const int sourceX = x * ctx->sourceWidth / ctx->targetWidth;
    ctx->targetRow[x] = luminance565(ctx->sourceRow[sourceX]);
  }
  std::fill(ctx->targetRow + ctx->targetWidth, ctx->targetRow + ctx->rowBytes, 0);

  const int firstTargetY = draw->y * ctx->targetHeight / ctx->sourceHeight;
  const int nextTargetY = (draw->y + 1) * ctx->targetHeight / ctx->sourceHeight;
  for (int targetY = firstTargetY; targetY < nextTargetY; ++targetY) {
    if (ctx->output->write(ctx->targetRow, static_cast<size_t>(ctx->rowBytes)) != ctx->rowBytes) {
      return 0;
    }
  }
  return 1;
}

bool pngFileToBmpStream(const std::string& sourcePath, const std::string& targetPath,
                        int width, int height) {
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
  if (sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > 2048 || sourceHeight > 3072 ||
      !writePngBmpHeader(output, width, height)) {
    output.close();
    decoder->close();
    return false;
  }

  const int rowBytes = (width + 3) & ~3;
  auto* targetRow = static_cast<uint8_t*>(M4Psram::mallocPrefer(static_cast<size_t>(rowBytes)));
  auto* sourceRow = static_cast<uint16_t*>(M4Psram::mallocPrefer(
      static_cast<size_t>(sourceWidth) * sizeof(uint16_t)));
  if (!targetRow || !sourceRow) {
    M4Psram::freePrefer(targetRow);
    M4Psram::freePrefer(sourceRow);
    output.close();
    decoder->close();
    return false;
  }

  PngBmpContext ctx{decoder.get(), &output, targetRow, sourceRow, sourceWidth, sourceHeight,
                    width, height, rowBytes};
  const bool ok = decoder->decode(&ctx, PNG_CHECK_CRC) == PNG_SUCCESS;
  M4Psram::freePrefer(targetRow);
  M4Psram::freePrefer(sourceRow);
  output.close();
  decoder->close();
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
    if (oneBit) return false;
    FsFile input;
    if (!SdMan.openFileForRead("M4Cover", source.c_str(), input)) return false;
    Bitmap bitmap(input);
    const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
    input.close();
    return valid && copyFile(source, target);
  }
  if (format == ImageFormat::Png) return pngFileToBmpStream(source, target, width, height);
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

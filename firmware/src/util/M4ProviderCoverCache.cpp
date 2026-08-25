#include "util/M4ProviderCoverCache.h"

#include <Bitmap.h>
#include <JpegToBmpConverter.h>
#include <SDCardManager.h>

#include <string>

#include "network/HttpDownloader.h"

namespace {

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
  backend.fetch = [](const std::string& url, const std::string& path, size_t maxBytes) {
    return HttpDownloader::downloadToFileBounded(url, path, maxBytes) == HttpDownloader::OK;
  };
  backend.convert = [](const std::string& source, const std::string& target, int width, int height) {
    FsFile sniff;
    if (!SdMan.openFileForRead("M4Cover", source.c_str(), sniff)) return false;
    const auto format = sniffFile(sniff);
    sniff.close();
    if (format == ImageFormat::Bmp) {
      FsFile input;
      if (!SdMan.openFileForRead("M4Cover", source.c_str(), input)) return false;
      Bitmap bitmap(input);
      const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
      input.close();
      return valid && copyFile(source, target);
    }
    if (format != ImageFormat::Jpeg) return false;
    FsFile input;
    FsFile output;
    if (!SdMan.openFileForRead("M4Cover", source.c_str(), input) ||
        !SdMan.openFileForWrite("M4Cover", target.c_str(), output)) {
      input.close();
      output.close();
      return false;
    }
    const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(input, output, width, height);
    input.close();
    output.close();
    return ok;
  };
  backend.remove = [](const std::string& path) { SdMan.remove(path.c_str()); };
  return acquire(request, backend);
}

}  // namespace M4ProviderCoverCache

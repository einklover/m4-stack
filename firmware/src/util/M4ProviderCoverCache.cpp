#include "util/M4ProviderCoverCache.h"

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

bool isJpegUrl(const std::string& url) {
  const size_t query = url.find_first_of("?#");
  const std::string path = url.substr(0, query == std::string::npos ? url.size() : query);
  if (path.size() < 4) return false;
  const std::string ext = path.substr(path.size() - 4);
  return (ext == ".jpg" || ext == ".JPG") ||
         (path.size() >= 5 && (path.substr(path.size() - 5) == ".jpeg" || path.substr(path.size() - 5) == ".JPEG"));
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
  backend.convert = [&request](const std::string& source, const std::string& target, int width, int height) {
    if (!isJpegUrl(request.coverUrl)) return copyFile(source, target);
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

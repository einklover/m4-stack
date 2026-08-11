#include "BootActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "CrossPointSettings.h"
#include "fontIds.h"
#include "util/CrosslinkDefaultWallpaper.h"
#include "../../lib/Epub/Epub/converters/ImageDecoderFactory.h"
#include "../../util/ImageCache.h"

// Try to render a BMP wallpaper from /sleep/ dir or /sleep.bmp.
// Writes to framebuffer, does NOT call displayBuffer().
// Returns true on success.
static bool tryRenderBmpWallpaper(GfxRenderer& renderer) {
  const int pw = renderer.getScreenWidth();
  const int ph = renderer.getScreenHeight();

  // 1. First valid .bmp from /sleep/ directory
  auto dir = SdMan.open("/sleep");
  if (dir && dir.isDirectory()) {
    char name[256];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) { file.close(); continue; }
      file.getName(name, sizeof(name));
      std::string filename = name;
      if (filename.empty() || filename[0] == '.' ||
          filename.length() < 4 || filename.substr(filename.length() - 4) != ".bmp") {
        file.close(); continue;
      }
      FsFile bmpFile;
      if (SdMan.openFileForRead("BOOT", "/sleep/" + filename, bmpFile)) {
        Bitmap bitmap(bmpFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.clearScreen();
          renderer.drawBitmap(bitmap, 0, 0, pw, ph, 0.0f, 0.0f);
          file.close();
          dir.close();
          return true;
        }
        bmpFile.close();
      }
      file.close();
    }
    dir.close();
  }

  // 2. /sleep.bmp at root
  FsFile bmpFile;
  if (SdMan.openFileForRead("BOOT", "/sleep.bmp", bmpFile)) {
    Bitmap bitmap(bmpFile, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderer.clearScreen();
      renderer.drawBitmap(bitmap, 0, 0, pw, ph, 0.0f, 0.0f);
      return true;
    }
  }
  return false;
}

// Try to render a PNG/JPG wallpaper from /lock_screen/ dir or root candidates.
// Uses .pxc pixel cache: cache hit → direct render; miss → decode + write cache.
// Writes to framebuffer, does NOT call displayBuffer().
// Returns true on success.
static bool tryRenderPngWallpaper(GfxRenderer& renderer) {
  RenderConfig rc;
  rc.x = 0; rc.y = 0;
  rc.maxWidth = renderer.getScreenWidth();
  rc.maxHeight = renderer.getScreenHeight();
  rc.useGrayscale = false;
  rc.useDithering = true;

  // Helper: render one image path with cache check.
  auto tryRenderOne = [&](const std::string& path) -> bool {
    uint32_t srcSize = ImageCache::getSourceSize(path);
    renderer.clearScreen();
    // Cache hit: render directly from .pxc (fast, no decode)
    if (ImageCache::isValid(path, srcSize) && ImageCache::renderFromCache(path, renderer)) {
      return true;
    }
    // Cache miss: decode and save .pxc for next boot
    rc.cachePath = ImageCache::getDecodeCachePath(path);
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(path);
    if (decoder && decoder->decodeToFramebuffer(path, renderer, rc)) {
      if (srcSize > 0) ImageCache::commit(path, srcSize);
      return true;
    }
    return false;
  };

  // 1. First valid image from /lock_screen/ directory
  auto dir = SdMan.open("/lock_screen");
  if (dir && dir.isDirectory()) {
    char name[256];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) { file.close(); continue; }
      file.getName(name, sizeof(name));
      std::string filename = name;
      if (filename.empty() || filename[0] == '.' ||
          !ImageDecoderFactory::isFormatSupported(filename)) {
        file.close(); continue;
      }
      file.close();
      if (tryRenderOne("/lock_screen/" + filename)) {
        dir.close();
        return true;
      }
    }
    dir.close();
  }

  // 2. Root candidates
  const char* rootCandidates[] = {"/lock_screen.png", "/lock_screen.jpg", "/lock_screen.jpeg"};
  for (const char* candidate : rootCandidates) {
    FsFile szFile;
    if (!SdMan.openFileForRead("BOOT", candidate, szFile)) continue;
    szFile.close();
    if (tryRenderOne(std::string(candidate))) {
      return true;
    }
  }
  return false;
}

void BootActivity::onEnter() {
  Activity::onEnter();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  bool customRendered = false;
  const uint8_t mode = SETTINGS.sleepScreen;

  // DARK (0) and LIGHT (1) are default modes → use built-in boot screen.
  // Other modes have custom wallpapers → render the same image as the sleep screen.
  if (mode == CrossPointSettings::CUSTOM || mode == CrossPointSettings::COVER_CUSTOM) {
    customRendered = tryRenderBmpWallpaper(renderer);
  } else if (mode == CrossPointSettings::MARSK2 || mode == CrossPointSettings::TRANSPARENT) {
    customRendered = tryRenderPngWallpaper(renderer);
  }
  // COVER, MARSK, BLANK, TRANSPARENT → fall through to default

  if (!customRendered) {
    // Built-in boot screen: background image
    drawCrosslinkDefaultWallpaper(renderer);
  }

  // Overlay boot text on top of wallpaper or default background
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}

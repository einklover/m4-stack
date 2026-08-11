#include "SleepActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "I18n.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/CrosslinkDefaultWallpaper.h"
#include "util/StringUtils.h"


#include "../../lib/Epub/Epub/converters/PngToFramebufferConverter.h"
#include "../../lib/Epub/Epub/converters/ImageDecoderFactory.h"
#include "../../lib/Epub/Epub/converters/DitherUtils.h"
#include "../../util/ImageCache.h"

// ── 灰阶 4-阶渲染（BW + LSB + MSB 三轮）────────────────────────────────────
// useHalfRefresh=true → 高清模式（HALF_REFRESH BW 基底，更稳定但较慢）
// useHalfRefresh=false → 正常模式（FAST_REFRESH BW 基底，速度更快）
// 首次: 解码 JPEG/PNG (useDithering=false) → 存入 _hd.pxc 缓存
// 后续: 直接从 _hd.pxc 三轮读取，无需重新解码
static bool renderImageHD(GfxRenderer& renderer, const std::string& filename, uint32_t srcSize,
                          bool useHalfRefresh = false) {
  bool hdHit = ImageCache::isHdValid(filename, srcSize);

  // ── BW base pass ──
  renderer.clearScreen();
  if (!hdHit) {
    // 首次：解码并缓存（无抖动 → 真实 4 级灰度值）
    RenderConfig rc;
    rc.x = 0; rc.y = 0;
    rc.maxWidth = renderer.getScreenWidth(); rc.maxHeight = renderer.getScreenHeight();
    rc.useDithering = false;
    rc.cachePath = ImageCache::getHdDecodeCachePath(filename);
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filename);
    if (!decoder || !decoder->decodeToFramebuffer(filename, renderer, rc)) {
      return false;
    }
    if (srcSize > 0) ImageCache::commitHd(filename, srcSize);
    hdHit = true;  // 缓存已写好，后续从缓存读取
  } else {
    if (!ImageCache::renderFromHdCache(filename, renderer)) return false;
  }
  // ── BW base pass（据 sleepBeforeFullRefresh 决定 FULL/HALF_REFRESH，清除阅读页残影）──
  renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);

  // ── GRAYSCALE_LSB pass ──
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  ImageCache::renderFromHdCache(filename, renderer);
  renderer.copyGrayscaleLsbBuffers();

  // ── GRAYSCALE_MSB pass ──
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  ImageCache::renderFromHdCache(filename, renderer);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

void SleepActivity::onEnter() {
  Activity::onEnter();

  // 关机前保存阅读统计
  READING_STATS.saveToFile();

  // TRANSPARENT 模式须保留 framebuffer（阅读内容），不能清屏
  // 其他模式：壁纸渲染时根据 sleepBeforeFullRefresh 决定是否 FULL_REFRESH 清除残影

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
    GUI.drawPopup(renderer, L(Str::kShuttingDown));
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
    GUI.drawPopup(renderer, L(Str::kShuttingDown));
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      GUI.drawPopup(renderer, L(Str::kShuttingDown));
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::MARSK):
    GUI.drawPopup(renderer, L(Str::kShuttingDown));
      return renderpngtxtSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::MARSK2):
      // 不调用 drawPopup：弹框的 displayBuffer 会把“关机中”写入屏幕，
      // 后续灰阶多遗渲染不一定能完全覆盖，导致文字残留
      return renderPngSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT):
      // !! 不调用 drawPopup !!
      // drawPopup 内部会 fillRect+displayBuffer，会清除 framebuffer 顶部区域并触发刷新，
      // 导致阅读页内容被覆盖。透明模式必须保留完整的阅读页 framebuffer。
      return renderTransparentSleepScreen();
    default:
    GUI.drawPopup(renderer, L(Str::kShuttingDown));
      return renderDefaultSleepScreen();
  }
}


void SleepActivity::renderpngtxtSleepScreen() const {
  bool isPngtxtLoaded = false; // 标记是否成功加载PNGTXT文件

  // ========== 分支1：优先从 /lock_screen 目录随机加载 .pngtxt ==========
  auto dir = SdMan.open("/lock_screen");
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    char name[256]; // 缩减文件名缓冲区长度（足够用）
    
    // 收集所有 .pngtxt 文件
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      std::string filename = name;
      
      // 跳过隐藏文件（.开头）
      if (!filename.empty() && filename[0] == '.') {
        file.close();
        continue;
      }

      // 修正：判断后缀为 .pngtxt
      const std::string suffix = ".pngtxt";
      if (filename.length() < suffix.length() || 
          filename.substr(filename.length() - suffix.length()) != suffix) {
        file.close();
        continue;
      }
      
      files.emplace_back(filename);
      file.close();
    }

    const size_t numFiles = files.size();
    if (numFiles > 0) {
      // 初始化随机数种子（确保每次随机结果不同）
      randomSeed(millis());
      // 生成 0 ~ numFiles-1 的随机索引（修正注释）
      size_t randomFileIndex = random(numFiles);
      // 避免重复加载同一张图（仅当文件数>1时）
      while (numFiles > 1 && randomFileIndex == APP_STATE.lastSleepImage) {
        randomFileIndex = random(numFiles);
      }
      APP_STATE.lastSleepImage = randomFileIndex;
      APP_STATE.saveToFile();

      const std::string filename = "/lock_screen/" + files[randomFileIndex];
      FsFile file;
      if (SdMan.openFileForRead("SLP", filename, file)) {
        
        // 绘制PNGTXT（灰阶分层绘制）
        renderer.drawPngFromTxtpng(filename.c_str());
        renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);

        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        renderer.drawPngFromTxtpng(filename.c_str());
        renderer.copyGrayscaleLsbBuffers();

        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        renderer.drawPngFromTxtpng(filename.c_str());
        renderer.copyGrayscaleMsbBuffers();

        renderer.displayGrayBuffer();
        renderer.setRenderMode(GfxRenderer::BW);

        GUI.drawPopup(renderer, L(Str::kSleeping));
        file.close(); // 关闭文件，避免泄漏
        isPngtxtLoaded = true; // 标记加载成功
      } else {
      }
    }
    dir.close(); // 无论是否加载成功，都关闭目录句柄
  } else if (dir) {
    dir.close(); // 目录打开失败时，关闭无效句柄
  }

  // ========== 分支2：若随机加载失败，加载单个 /sleep.pngtxt ==========
  if (!isPngtxtLoaded) {
    const std::string pngtxtPath = "/sleep.pngtxt";
    FsFile txtpng_file;
    if (SdMan.openFileForRead("GFD", pngtxtPath, txtpng_file)) {
      
      // 绘制PNGTXT（灰阶分层绘制）
      renderer.drawPngFromTxtpng(pngtxtPath.c_str());
      renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderer.drawPngFromTxtpng(pngtxtPath.c_str());
      renderer.copyGrayscaleLsbBuffers();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderer.drawPngFromTxtpng(pngtxtPath.c_str());
      renderer.copyGrayscaleMsbBuffers();

      renderer.displayGrayBuffer();
      renderer.setRenderMode(GfxRenderer::BW);

      txtpng_file.close(); // 关闭文件，避免泄漏
      isPngtxtLoaded = true; // 标记加载成功
    } else {
    }
  }

  // ========== 分支3：仅当所有PNGTXT加载失败时，才绘制默认睡眠屏 ==========
  if (!isPngtxtLoaded) {
    renderDefaultSleepScreen();
  }
}


void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /sleep directory
  auto dir = SdMan.open("/sleep");
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      if (filename.substr(filename.length() - 4) != ".bmp") {
        file.close();
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        file.close();
        continue;
      }
      files.emplace_back(filename);
      file.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Generate a random number between 1 and numFiles
      auto randomFileIndex = random(numFiles);
      // If we picked the same image as last time, reroll
      while (numFiles > 1 && randomFileIndex == APP_STATE.lastSleepImage) {
        randomFileIndex = random(numFiles);
      }
      APP_STATE.lastSleepImage = randomFileIndex;
      APP_STATE.saveToFile();
      const auto filename = "/sleep/" + files[randomFileIndex];
      FsFile file;
      if (SdMan.openFileForRead("SLP", filename, file)) {
        delay(100);
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          dir.close();
          return;
        }
      }
    }
  }
  if (dir) dir.close();

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (SdMan.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}


// ── PNG透明4灰阶渲染：直解PNG三轮，正确保留阅读内容并实现4级灰度 ──────────────
// 原理与 renderTransparentSleepScreen 相同，但用PNG直解替代.pxc，
// 彻底避免 pxc value=3 同时代表「近白色不透明」和「alpha=0透明」的语义冲突。
// BW  pass: 不清屏 → alpha=0跳过(背景/阅读内容透出), alpha>0叠加
// LSB pass: 不清屏 → 再次解码，仅value=1生效 → copyGrayscaleLsbBuffers
// MSB pass: 不清屏 → 再次解码，value=1,2生效 → copyGrayscaleMsbBuffers
// displayGrayBuffer() → 真正4级灰度(黑/深灰/浅灰/白)正确显示
static void renderPngTransparentHD(GfxRenderer& renderer, const std::string& pngPath,
                                   int maxW, int maxH) {
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(pngPath);
  if (!decoder) return;

  RenderConfig rc;
  rc.x = 0; rc.y = 0;
  rc.maxWidth = maxW;
  rc.maxHeight = maxH;
  rc.useDithering = true;

  // Pass 1 (BW)：不清屏，将PNG叠加到阅读内容上，然后必须先显示建立基底
  if (!decoder->decodeToFramebuffer(pngPath, renderer, rc)) return;
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  delay(200);

  // Pass 2 (LSB)：clearScreen(0x00)，只将 value=1 像素写入BW RAM
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  decoder->decodeToFramebuffer(pngPath, renderer, rc);
  renderer.copyGrayscaleLsbBuffers();

  // Pass 3 (MSB)：clearScreen(0x00)，只将 value=1,2 像素写入RED RAM
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  decoder->decodeToFramebuffer(pngPath, renderer, rc);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

// ── 以下函数已废弃，保留仅供参考 ────────────────────────────────────────────
[[maybe_unused]] static void renderPxcTransparent4Gray_UNUSED(GfxRenderer& renderer, const std::string& pxcPath) {
  FsFile ovFile;
  if (!SdMan.openFileForRead("SLP-4G", pxcPath.c_str(), ovFile)) return;

  uint16_t ovW = 0, ovH = 0;
  if (ovFile.read(&ovW, 2) != 2 || ovFile.read(&ovH, 2) != 2) {
    ovFile.close(); return;
  }

  const int bpr = (ovW + 3) / 4;
  auto* row = (uint8_t*)malloc(bpr);
  if (!row) { ovFile.close(); return; }

  const int drawW = ((int)ovW < renderer.getScreenWidth())  ? (int)ovW : renderer.getScreenWidth();
  const int drawH = ((int)ovH < renderer.getScreenHeight()) ? (int)ovH : renderer.getScreenHeight();
  const uint32_t dataOffset = 4;

  auto overlayPass = [&]() {
    ovFile.seek(dataOffset);
    for (int y = 0; y < drawH; y++) {
      if (ovFile.read(row, bpr) != bpr) break;
      for (int x = 0; x < drawW; x++) {
        uint8_t pv = (row[x / 4] >> (6 - (x % 4) * 2)) & 0x03;
        if (pv < 3) drawPixelWithRenderMode(renderer, x, y, pv);  // 值3=透明，跳过
      }
    }
  };

  // BW pass: framebuffer(阅读内容) + overlay 非透明像素
  overlayPass();
  delay(200);

  // LSB pass
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  overlayPass();
  renderer.copyGrayscaleLsbBuffers();

  // MSB pass
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  overlayPass();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  free(row);
  ovFile.close();
}

void SleepActivity::renderPngSleepScreen() const {

  auto dir = SdMan.open("/lock_screen");
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    char name[500];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) { file.close(); continue; }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') { file.close(); continue; }
      if (!ImageDecoderFactory::isFormatSupported(filename)) { file.close(); continue; }
      files.emplace_back(filename);
      file.close();
    }

    const auto numFiles = files.size();
    if (numFiles > 0) {
      auto randomFileIndex = random(numFiles);
      const std::string filename = "/lock_screen/" + files[randomFileIndex];

      uint32_t srcSize = ImageCache::getSourceSize(filename);
      bool cacheHit = ImageCache::isValid(filename, srcSize);

      // ── 判断是否为PNG格式 ──
      bool isPng = StringUtils::checkFileExtension(filename, ".png");

      if (isPng) {
        // PNG格式：走现有的透明叠加流程
        // ── 透明叠加模式（sleepPngInvert==0）：不清屏，直接解码 PNG ──
        // alpha=0 像素不绘制（阅读内容透出），alpha>0 像素叠加显示
        // 注：必须绕过 .pxc 缓存（value=3 无法区分近白色不透明与真透明）
        if (SETTINGS.sleepPngInvert == 0) {
          if (SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL) {
            // 普通/高清：PNG直解三轮，真正4灰阶透明叠加（0=黑/1=深灰/2=浅灰/3=白）
            renderPngTransparentHD(renderer, filename, renderer.getScreenWidth(), renderer.getScreenHeight());
          } else {
            // 快速：BW直接解码（不清屏，无抖动，2色阶）
            RenderConfig rc;
            rc.x = 0; rc.y = 0;
            rc.maxWidth = renderer.getScreenWidth(); rc.maxHeight = renderer.getScreenHeight();
            rc.useDithering = true;
            ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filename);
            if (decoder && decoder->decodeToFramebuffer(filename, renderer, rc)) {
              renderer.displayBuffer(HalDisplay::HALF_REFRESH);
            }
          }
          dir.close();
          return;
        }

        // ── 灰阶模式优先（正常/高清，忽略 sleepPngInvert）──
        if (SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL) {
          bool useHalf = (SETTINGS.imageQuality == CrossPointSettings::QUALITY_HD);
          if (renderImageHD(renderer, filename, srcSize, useHalf)) {
            dir.close();
            return;
          }
        }

        renderer.clearScreen();
        if (cacheHit) {
          // 缓存命中：直接从 .pxc 渲染
          if (ImageCache::renderFromCache(filename, renderer)) {
            if (SETTINGS.sleepPngInvert) renderer.invertScreen();
            renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
            dir.close();
            return;
          }
        }

        // 缓存未命中或损坏：解码，同时写入 .pxc 缓存
        delay(100);
        RenderConfig rc;
        rc.x = 0; rc.y = 0;
        rc.maxWidth = renderer.getScreenWidth(); rc.maxHeight = renderer.getScreenHeight();
        rc.useDithering = true;
        rc.cachePath = ImageCache::getDecodeCachePath(filename);

        ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filename);
        if (decoder && decoder->decodeToFramebuffer(filename, renderer, rc)) {
          if (SETTINGS.sleepPngInvert) renderer.invertScreen();
          renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
          if (srcSize > 0) ImageCache::commit(filename, srcSize);
          dir.close();
          return;
        }
      } else {
        // 非PNG格式（JPG等）：走参考文件的标准流程（清屏→渲染→invertScreen）
        // ── 灰阶模式优先（正常/高清，忽略 sleepPngInvert）──
        if (SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL) {
          bool useHalf = (SETTINGS.imageQuality == CrossPointSettings::QUALITY_HD);
          if (renderImageHD(renderer, filename, srcSize, useHalf)) {
            dir.close();
            return;
          }
        }

        renderer.clearScreen();
        if (cacheHit) {
          // 缓存命中：直接从 .pxc 渲染
          if (ImageCache::renderFromCache(filename, renderer)) {
            if (SETTINGS.sleepPngInvert) renderer.invertScreen();
            renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
            dir.close();
            return;
          }
        }

        // 缓存未命中或损坏：解码，同时写入 .pxc 缓存
        delay(100);
        RenderConfig rc;
        rc.x = 0; rc.y = 0;
        rc.maxWidth = renderer.getScreenWidth(); rc.maxHeight = renderer.getScreenHeight();
        rc.useDithering = true;
        rc.cachePath = ImageCache::getDecodeCachePath(filename);

        ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filename);
        if (decoder && decoder->decodeToFramebuffer(filename, renderer, rc)) {
          if (SETTINGS.sleepPngInvert) renderer.invertScreen();
          renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
          if (srcSize > 0) ImageCache::commit(filename, srcSize);
          dir.close();
          return;
        }
      }
    }
  }
  if (dir) dir.close();

  // 根目录单文件候选（支持 png / jpg）
  const char* rootCandidates[] = {"/lock_screen.png", "/lock_screen.jpg", "/lock_screen.jpeg"};
  for (const char* candidate : rootCandidates) {
    FsFile szFile;
    if (!SdMan.openFileForRead("SLP", candidate, szFile)) continue;
    uint32_t srcSize = static_cast<uint32_t>(szFile.size());
    szFile.close();

    const std::string candPath(candidate);

    // ── 判断是否为PNG格式 ──
    bool isPng = StringUtils::checkFileExtension(candPath, ".png");

    if (isPng) {
      // PNG格式：走现有的透明叠加流程
      // ── 透明叠加模式（sleepPngInvert==0）：不清屏，直接解码 PNG ──
      if (SETTINGS.sleepPngInvert == 0) {
        if (SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL) {
          renderPngTransparentHD(renderer, candPath, renderer.getScreenWidth(), renderer.getScreenHeight());
        } else {
          RenderConfig rc;
          rc.x = 0; rc.y = 0;
          rc.maxWidth = renderer.getScreenWidth(); rc.maxHeight = renderer.getScreenHeight();
          rc.useDithering = true;
          ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(candPath);
          if (decoder && decoder->decodeToFramebuffer(candPath, renderer, rc)) {
            renderer.displayBuffer(HalDisplay::HALF_REFRESH);
          }
        }
        return;
      }

      // ── 灰阶模式优先 ──
      if (SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL) {
        bool useHalf = (SETTINGS.imageQuality == CrossPointSettings::QUALITY_HD);
        if (renderImageHD(renderer, candPath, srcSize, useHalf)) {
          return;
        }
      }

      renderer.clearScreen();

      if (ImageCache::isValid(candPath, srcSize)) {
        if (ImageCache::renderFromCache(candPath, renderer)) {
          if (SETTINGS.sleepPngInvert) renderer.invertScreen();
          renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
          return;
        }
      }

      delay(100);
      RenderConfig rc;
      rc.x = 0; rc.y = 0;
      rc.maxWidth = renderer.getScreenWidth(); rc.maxHeight = renderer.getScreenHeight();
      rc.useDithering = true;
      rc.cachePath = ImageCache::getDecodeCachePath(candPath);

      ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(candPath);
      if (decoder && decoder->decodeToFramebuffer(candPath, renderer, rc)) {
        if (SETTINGS.sleepPngInvert) renderer.invertScreen();
        renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
        if (srcSize > 0) ImageCache::commit(candPath, srcSize);
        return;
      }
    } else {
      // 非PNG格式（JPG等）：走参考文件的标准流程
      // ── 灰阶模式优先 ──
      if (SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL) {
        bool useHalf = (SETTINGS.imageQuality == CrossPointSettings::QUALITY_HD);
        if (renderImageHD(renderer, candPath, srcSize, useHalf)) {
          return;
        }
      }

      renderer.clearScreen();

      if (ImageCache::isValid(candPath, srcSize)) {
        if (ImageCache::renderFromCache(candPath, renderer)) {
          if (SETTINGS.sleepPngInvert) renderer.invertScreen();
          renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
          return;
        }
      }

      delay(100);
      RenderConfig rc;
      rc.x = 0; rc.y = 0;
      rc.maxWidth = renderer.getScreenWidth(); rc.maxHeight = renderer.getScreenHeight();
      rc.useDithering = true;
      rc.cachePath = ImageCache::getDecodeCachePath(candPath);

      ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(candPath);
      if (decoder && decoder->decodeToFramebuffer(candPath, renderer, rc)) {
        if (SETTINGS.sleepPngInvert) renderer.invertScreen();
        renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
        if (srcSize > 0) ImageCache::commit(candPath, srcSize);
        return;
      }
    }
  }

  renderDefaultSleepScreen();
}




void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  drawCrosslinkDefaultWallpaper(renderer);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, L(Str::kPoweredOff));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}



void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtc") ||
      StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtch")) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".txt")) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".epub")) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (SdMan.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(SETTINGS.sleepBeforeFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH);
}

// ── 透明叠加关机壁纸 ────────────────────────────────────────────────────────
// 原理：不清除 framebuffer（保留阅读页内容），将覆盖层 .pxc 四灰阶叠加上去
// BW pass  : framebuffer(阅读内容) + overlay 非白像素（drawPixelWithRenderMode BW）
// LSB pass : clearScreen(0x00) + overlay value=1 像素（reading content 不需重画）
// MSB pass : clearScreen(0x00) + overlay value=1,2 像素
// 内存仅需 ~120 字节（一行行缓冲）
void SleepActivity::renderTransparentSleepScreen() const {
  const char* pxcPath = SETTINGS.transparentOverlayPxc;

  if (pxcPath[0] == '\0') {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  FsFile ovFile;
  if (!SdMan.openFileForRead("SLP-TR", pxcPath, ovFile)) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  uint16_t ovW = 0, ovH = 0;
  if (ovFile.read(&ovW, 2) != 2 || ovFile.read(&ovH, 2) != 2) {
    ovFile.close();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  const int bpr = (ovW + 3) / 4;
  auto* row = (uint8_t*)malloc(bpr);
  if (!row) {
    ovFile.close();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  const int drawW = ((int)ovW < renderer.getScreenWidth())  ? (int)ovW : renderer.getScreenWidth();
  const int drawH = ((int)ovH < renderer.getScreenHeight()) ? (int)ovH : renderer.getScreenHeight();
  const uint32_t dataOffset = 4;
  const bool removeWhite = SETTINGS.transparentRemoveWhite;  // 是否跳过白色像素

  // 内联 helper：按当前 renderMode 叠加 .pxc 到 framebuffer
  // removeWhite=true: 白色像素(pv=3)跳过（透明效果）
  // removeWhite=false: 白色像素也绘制为白色（在四灰阶中白色是背景，无需绘制）
  auto overlayPass = [&]() {
    ovFile.seek(dataOffset);
    for (int y = 0; y < drawH; y++) {
      if (ovFile.read(row, bpr) != bpr) break;
      for (int x = 0; x < drawW; x++) {
        uint8_t pv = (row[x / 4] >> (6 - (x % 4) * 2)) & 0x03;
        if (removeWhite) {
          // 透明模式：白色像素(pv=3)跳过，其他像素绘制
          if (pv < 3) {
            drawPixelWithRenderMode(renderer, x, y, pv);
          }
        } else {
          // 非透明模式：黑色/灰色像素绘制，白色像素(pv=3)也绘制（在BW中白色就是清除）
          if (pv < 3) {
            drawPixelWithRenderMode(renderer, x, y, pv);
          } else {
            // pv=3 白色：在非透明模式下，如果当前是BW模式，需要清除该像素的黑色
            // 因为白色应该覆盖阅读内容的黑色
            if (renderer.getRenderMode() == GfxRenderer::BW) {
              renderer.drawPixel(x, y, false);  // 清除黑色，显示白色
            }
          }
        }
      }
    }
  };

  // ── BW pass：阅读内容（framebuffer）+ overlay 非白像素 ───────────────────
  overlayPass();
  // renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  delay(200);

  // ── LSB pass：overlay value=1 像素（阅读内容不需重画）─────────────────────
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  overlayPass();
  renderer.copyGrayscaleLsbBuffers();

  // ── MSB pass：overlay value=1,2 像素 ─────────────────────────────────────
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  overlayPass();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  free(row);
  ovFile.close();
}

#include "PngToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <PNGdec.h>
#include <SDCardManager.h>
#include <SdFat.h>

#include <cstdlib>
#include <new>

#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the FsFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer;
  const RenderConfig* config;
  int screenWidth;
  int screenHeight;

  // Scaling state
  float scale;
  int srcWidth;
  int srcHeight;
  int dstWidth;
  int dstHeight;
  int lastDstY;  // Track last rendered destination Y to avoid duplicates

  PixelCache cache;
  bool caching;

  uint8_t* grayLineBuffer;
  uint8_t* alphaLineBuffer; // 仅新增：Alpha通道缓冲（用于判断透明）

  PngContext()
      : renderer(nullptr),
        config(nullptr),
        screenWidth(0),
        screenHeight(0),
        scale(1.0f),
        srcWidth(0),
        srcHeight(0),
        dstWidth(0),
        dstHeight(0),
        lastDstY(-1),
        caching(false),
        grayLineBuffer(nullptr),
        alphaLineBuffer(nullptr) {} // 仅初始化新增的Alpha缓冲
};

// File I/O callbacks use pFile->fHandle to access the FsFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!SdMan.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// The PNG decoder (PNGdec) is ~42 KB due to internal zlib decompression buffers.
// We heap-allocate it on demand rather than using a static instance, so this memory
// is only consumed while actually decoding/querying PNG images. This is critical on
// the ESP32-C3 where total RAM is ~320 KB.
constexpr size_t PNG_DECODER_APPROX_SIZE = 44 * 1024;                          // ~42 KB + overhead
constexpr size_t MIN_FREE_HEAP_FOR_PNG = PNG_DECODER_APPROX_SIZE + 16 * 1024;  // decoder + 16 KB headroom

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required SdMan is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.
int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int requiredPngInternalBufferBytes(int srcWidth, int pixelType) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  return ((pitch + 1) * 2) + 32;
}

// 仅修改：拆分灰度和Alpha，保留原有灰度计算逻辑，单独提取Alpha值
void convertLineToGrayAndAlpha(uint8_t* pPixels, uint8_t* grayLine, uint8_t* alphaLine, int width, int pixelType, uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      memcpy(grayLine, pPixels, width);
      memset(alphaLine, 255, width); // 无Alpha=完全不透明
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 3];
        // 保留原有灰度计算逻辑
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        alphaLine[x] = 255; // 无Alpha=完全不透明
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = pPixels[x];
            uint8_t* p = &palette[idx * 3];
            // 保留原有灰度计算逻辑
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            grayLine[x] = gray;
            alphaLine[x] = palette[768 + idx]; // 单独提取Alpha值
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t* p = &palette[pPixels[x] * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            alphaLine[x] = 255;
          }
        }
      } else {
        memcpy(grayLine, pPixels, width);
        memset(alphaLine, 255, width);
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        // 保留原有灰度计算逻辑（和透明混合）
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
        alphaLine[x] = alpha; // 单独提取Alpha值
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        // 保留原有灰度计算逻辑（和透明混合）
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
        alphaLine[x] = alpha; // 单独提取Alpha值
      }
      break;

    default:
      memset(grayLine, 128, width);
      memset(alphaLine, 255, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer || !ctx->alphaLineBuffer) return 0;

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  // Calculate destination Y with scaling
  int dstY = (int)(srcY * ctx->scale);

  // Skip if we already rendered this destination row (multiple source rows map to same dest)
  if (dstY == ctx->lastDstY) return 1;
  ctx->lastDstY = dstY;

  // Check bounds
  if (dstY >= ctx->dstHeight) return 1;

  int outY = ctx->config->y + dstY;
  if (outY >= ctx->screenHeight) return 1;

  // 仅修改：调用拆分后的灰度+Alpha转换函数
  convertLineToGrayAndAlpha(pDraw->pPixels, ctx->grayLineBuffer, ctx->alphaLineBuffer,
                           srcWidth, pDraw->iPixelType, pDraw->pPalette, pDraw->iHasAlpha);

  // Render scaled row using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;
  bool caching = ctx->caching;

  int srcX = 0;
  int error = 0;

  for (int dstX = 0; dstX < dstWidth; dstX++) {
    int outX = outXBase + dstX;
    if (outX < screenWidth) {
      // 防护：srcX不能越界
      if (srcX >= srcWidth) break;

      uint8_t gray = ctx->grayLineBuffer[srcX];
      uint8_t alpha = ctx->alphaLineBuffer[srcX]; // 读取单独的Alpha值

      // PNG透明像素(alpha=0)存为值3(透明标记)，白色像素也存为值3
      // 渲染时根据设置决定是否跳过值3
      if (alpha == 0) {
        // PNG透明像素：存为值3(透明)
        if (caching) ctx->cache.setPixel(outX, outY, 3);
        // 不绘制到framebuffer，保持底层内容
      } else if (alpha > 0) {
          // 获取渲染模式（和Bitmap逻辑对齐）
          auto renderMode = ctx->renderer->getRenderMode();
          //先清空区域
          if (renderMode == GfxRenderer::BW) {
              ctx->renderer->drawPixel(outX, outY, false);
          }


        uint8_t ditheredGray;
        if (useDithering) {
          ditheredGray = applyBayerDither4Level(gray, outX, outY);
        } else {
          // 4级阈值量化（与 applyBayerDither4Level 的阈值一致，不加抖动偏移）
          ditheredGray = (gray < 64 ? 0 : gray < 128 ? 1 : gray < 192 ? 2 : 3);
        }
        drawPixelWithRenderMode(*ctx->renderer, outX, outY, ditheredGray);
        if (caching) ctx->cache.setPixel(outX, outY, ditheredGray);
      }
    }

    // Bresenham-style stepping: advance srcX based on ratio srcWidth/dstWidth
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }

  return 1;
}

// ── RGBA pixel buffer context & callback (用于透明壁纸叠加) ─────────────────
struct PngPixelBufContext {
  uint8_t* rgbaBuf;
  int bufW, bufH, dstW, dstH, srcWidth, srcHeight;
  float scale;
  int lastDstY;
  uint8_t* grayLineBuf;
  uint8_t* alphaLineBuf;
};

}  // namespace

// Forward declare outside namespace so PNG::open can resolve the function pointer type
int pngPixelBufDrawCallback(PNGDRAW* pDraw);

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    Serial.printf("[PNG] Not enough heap for PNG decoder (%u free, need %u)\n", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  PNG* png = new (std::nothrow) PNG();
  if (!png) {
    Serial.printf("[PNG] Failed to allocate PNG decoder for dimensions\n");
    return false;
  }

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     nullptr);

  if (rc != 0) {
    Serial.printf("[PNG] Failed to open PNG for dimensions: %d\n", rc);
    delete png;
    return false;
  }

  out.width = png->getWidth();
  out.height = png->getHeight();

  png->close();
  delete png;
  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  Serial.printf("[PNG] Decoding PNG: %s\n", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    Serial.printf("[PNG] Not enough heap for PNG decoder (%u free, need %u)\n", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  // Heap-allocate PNG decoder (~42 KB) - freed at end of function
  PNG* png = new (std::nothrow) PNG();
  if (!png) {
    Serial.printf("[PNG] Failed to allocate PNG decoder\n");
    return false;
  }

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    Serial.printf("[PNG] Failed to open PNG: %d\n", rc);
    delete png;
    return false;
  }

  if (!validateImageDimensions(png->getWidth(), png->getHeight(), "PNG")) {
    png->close();
    delete png;
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = png->getWidth();
  ctx.srcHeight = png->getHeight();

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking

  Serial.printf("[PNG] PNG %dx%d -> %dx%d (scale %.2f), bpp: %d\n", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth, ctx.dstHeight,
          ctx.scale, png->getBpp());

  const int pixelType = png->getPixelType();
  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    Serial.printf("[PNG] PNG row buffer too small: need %d bytes for width=%d type=%d, configured PNG_MAX_BUFFERED_PIXELS=%d\n",
            requiredInternal, ctx.srcWidth, pixelType, PNG_MAX_BUFFERED_PIXELS);
    Serial.printf("[PNG] Aborting decode to avoid PNGdec internal buffer overflow\n");
    png->close();
    delete png;
    return false;
  }

  if (png->getBpp() != 8) {
    warnUnsupportedFeature("bit depth (" + std::to_string(png->getBpp()) + "bpp)", imagePath);
  }

  // 仅修改：分配Alpha缓冲，和灰度缓冲同大小
  const size_t grayBufSize = PNG_MAX_BUFFERED_PIXELS / 2;
  ctx.grayLineBuffer = static_cast<uint8_t*>(malloc(grayBufSize));
  ctx.alphaLineBuffer = static_cast<uint8_t*>(malloc(grayBufSize));
  if (!ctx.grayLineBuffer || !ctx.alphaLineBuffer) {
    Serial.printf("[PNG] Failed to allocate gray/alpha line buffer\n");
    free(ctx.grayLineBuffer);
    free(ctx.alphaLineBuffer);
    png->close();
    delete png;
    return false;
  }

  // Allocate cache buffer using SCALED dimensions
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.allocate(ctx.dstWidth, ctx.dstHeight, config.x, config.y)) {
      Serial.printf("[PNG] Failed to allocate cache buffer, continuing without caching\n");
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  rc = png->decode(&ctx, 0);
  unsigned long decodeTime = millis() - decodeStart;

  // 仅修改：释放Alpha缓冲
  free(ctx.grayLineBuffer);
  free(ctx.alphaLineBuffer);
  ctx.grayLineBuffer = nullptr;
  ctx.alphaLineBuffer = nullptr;

  if (rc != PNG_SUCCESS) {
    Serial.printf("[PNG] Decode failed: %d\n", rc);
    png->close();
    delete png;
    return false;
  }

  png->close();
  delete png;
  Serial.printf("[PNG] PNG decoding complete - render time: %lu ms\n", decodeTime);

  // Write cache file if caching was enabled and buffer was allocated
  if (ctx.caching) {
    ctx.cache.writeToFile(config.cachePath);
  }

  return true;
}



bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  std::string ext = extension;
  for (auto& c : ext) {
    c = tolower(c);
  }
  return (ext == ".png");
}

// ── RGBA pixel buffer decoder implementation (用于透明壁纸叠加) ───────────────

int pngPixelBufDrawCallback(PNGDRAW* pDraw) {
  PngPixelBufContext* ctx = reinterpret_cast<PngPixelBufContext*>(pDraw->pUser);
  if (!ctx || !ctx->rgbaBuf) return 0;

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  int dstY = (int)(srcY * ctx->scale);
  if (dstY == ctx->lastDstY) return 1;
  ctx->lastDstY = dstY;
  if (dstY >= ctx->dstH) return 1;

  // Convert raw pixels to grayscale + alpha
  convertLineToGrayAndAlpha(pDraw->pPixels, ctx->grayLineBuf, ctx->alphaLineBuf,
                            srcWidth, pDraw->iPixelType, pDraw->pPalette, pDraw->iHasAlpha);

  // Write scaled row to RGBA buffer
  int dstWidth = ctx->dstW;
  int error = 0;
  int srcX = 0;
  for (int dstX = 0; dstX < dstWidth; dstX++) {
    if (srcX >= srcWidth) break;

    uint8_t gray = ctx->grayLineBuf[srcX];
    uint8_t alpha = ctx->alphaLineBuf[srcX];

    // RGBA: white background with black/dark gray foreground
    if (alpha == 0 || gray >= 240) {
      // Transparent or near-white → write white (RGBA: 255,255,255,255)
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 0] = 255;
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 1] = 255;
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 2] = 255;
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 3] = 255;
    } else {
      // Opaque pixel → write gray as grayscale RGB
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 0] = gray;
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 1] = gray;
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 2] = gray;
      ctx->rgbaBuf[(dstY * ctx->bufW + dstX) * 4 + 3] = 255;
    }

    // Bresenham stepping
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }
  return 1;
}

size_t PngToFramebufferConverter::decodeToPixelBuf(const std::string& imagePath, uint8_t* rgbaBuf, int bufW, int bufH,
                                                   int& outW, int& outH) {
  PNG* png = new (std::nothrow) PNG();
  if (!png) return 0;

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle, nullptr);
  if (rc != 0) {
    png->close();
    delete png;
    return 0;
  }

  int srcW = png->getWidth();
  int srcH = png->getHeight();
  png->close();

  float scaleX = (float)bufW / srcW;
  float scaleY = (float)bufH / srcH;
  float scale = std::min(scaleX, scaleY);
  int dstW = (int)(srcW * scale);
  int dstH = (int)(srcH * scale);

  PngPixelBufContext ctx = {};
  ctx.rgbaBuf = rgbaBuf;
  ctx.bufW = bufW;
  ctx.bufH = bufH;
  ctx.dstW = dstW;
  ctx.dstH = dstH;
  ctx.scale = scale;
  ctx.lastDstY = -1;
  ctx.srcWidth = srcW;
  ctx.srcHeight = srcH;
  ctx.grayLineBuf = (uint8_t*)malloc(srcW);
  ctx.alphaLineBuf = (uint8_t*)malloc(srcW);
  if (!ctx.grayLineBuf || !ctx.alphaLineBuf) {
    free(ctx.grayLineBuf);
    free(ctx.alphaLineBuf);
    delete png;
    return 0;
  }

  png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
            pngPixelBufDrawCallback);
  rc = png->decode(&ctx, 0);
  png->close();
  delete png;

  free(ctx.grayLineBuf);
  free(ctx.alphaLineBuf);

  if (rc == PNG_SUCCESS) {
    outW = dstW;
    outH = dstH;
    return (size_t)dstW * dstH;
  }
  return 0;
}

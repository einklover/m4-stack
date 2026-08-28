#pragma once
#include <SdFat.h>

#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // Decode image into a pre-allocated RGBA pixel buffer (for compositing).
  // Out dimensions returned via outW/outH. Returns pixel count written.
  virtual size_t decodeToPixelBuf(const std::string& imagePath, uint8_t* rgbaBuf, int bufW, int bufH,
                                  int& outW, int& outH) = 0;

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
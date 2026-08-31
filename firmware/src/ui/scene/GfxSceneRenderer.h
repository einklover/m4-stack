#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

#include "ui/scene/UiSceneAssets.h"
#include "ui/scene/UiSceneRuntime.h"
#include "ui/scene/UiScenePackage.h"
#include "fontIds.h"
#include "util/M4FixedRuntimeUiFonts.h"

namespace UiScene {

// Generic scene -> GfxRenderer adapter.
// No SD, no network, no provider, no Lua, no JSON, no allocation.
// All assets are already-resolved immutable 1-bit buffers (UiSceneAssets)
// or M4TH package ASSET_DATA (for bitmap overlay). White is transparent.
//
// Render path is pure: it only reads package bytes via pgm_read_byte,
// reads snapshot via SceneBindingSource (already stable), reads assets,
// and emits gfx.draw* calls.
class GfxSceneRenderer {
 public:
  static AssetKey assetKey(const UiSceneRuntime::RenderEvent& event) {
    return {event.assetBinding,
            event.item.valid ? event.item.sourceBinding : kInvalidBindingId,
            event.item.valid ? event.item.index : kInvalidAssetItemIndex};
  }
  static int runtimeFontId(uint8_t sceneFontId) {
    switch (sceneFontId) {
      case 12: return UI_12_FONT_ID;
      case 13: return UI_12_FONT_ID;
      case 14:
      case 15: return NOTOSANS_14_FONT_ID;
      case 16:
      case 17: return NOTOSANS_16_FONT_ID;
      case 18:
      case 19: return NOTOSANS_18_FONT_ID;
      case 20:
      case 21: return M4FixedRuntimeUiFonts::kHubTitleFontId; // Hub title 20px free
      case 22:
      case 23: return M4FixedRuntimeUiFonts::kHubCategoryFontId; // Hub category 24px free
      case 24:
      case 25: return M4FixedRuntimeUiFonts::kHubCategoryFontId; // ui_24 also 24px free
      default: return sceneFontId;
    }
  }

  template <typename Gfx>
  static int16_t alignedTextX(const Gfx& gfx,
                              const UiSceneRuntime::RenderEvent& event,
                              const char* text) {
    if (event.align == 0 || event.rect.width == 0) return event.rect.x;
    int width = gfx.getTextWidth(runtimeFontId(event.font), text);
    if (width < 0) width = 0;
    if (width > event.rect.width) width = event.rect.width;
    const int remaining = static_cast<int>(event.rect.width) - width;
    return static_cast<int16_t>(event.rect.x +
        (event.align == 2 ? remaining : remaining / 2));
  }

  static size_t utf8CharBytes(const char* s) {
    if (!s || s[0] == '\0') return 0;
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
  }

  template <typename Gfx>
  static size_t fitUtf8Prefix(const Gfx& gfx, int fontId, const char* text,
                              size_t len, int maxW) {
    if (!text || len == 0 || maxW <= 0) return 0;
    char tmp[256];
    size_t best = 0;
    size_t i = 0;
    while (i < len && text[i] != '\0') {
      const size_t n = utf8CharBytes(text + i);
      if (n == 0 || i + n > len) break;
      const size_t next = i + n;
      if (next >= sizeof(tmp)) break;
      memcpy(tmp, text, next);
      tmp[next] = '\0';
      if (gfx.getTextWidth(fontId, tmp) > maxW) break;
      best = next;
      i = next;
    }
    return best;
  }

  // Wrap at most 2 lines inside ev.rect. Ellipsis only when a 2-line slot
  // still cannot hold the remaining glyphs (or a 1-line slot overflows).
  template <typename Gfx>
  static void drawSceneText(const Gfx& gfx,
                            const UiSceneRuntime::RenderEvent& ev,
                            const char* buf) {
    if (!buf || buf[0] == '\0') return;
    const int fontId = runtimeFontId(ev.font);
    const int maxW = ev.rect.width;
    int lineH = gfx.getLineHeight(fontId);
    if (lineH <= 0) lineH = 16;
    int maxLines = 1;
    if (ev.rect.height > 20 &&
        (ev.rect.height >= lineH * 2 || ev.rect.height >= 36)) {
      maxLines = 2;
    }
    const int fullW = gfx.getTextWidth(fontId, buf);
    if (maxW <= 0 || fullW <= maxW || maxLines <= 1) {
      if (ev.ellipsis && maxW > 0 && fullW > maxW) {
        char lineBuf[256];
        static const char kEllipsis[] = "\xE2\x80\xA6";
        const int ellW = gfx.getTextWidth(fontId, kEllipsis);
        const int budget = maxW > ellW ? maxW - ellW : 0;
        size_t take = fitUtf8Prefix(gfx, fontId, buf, strlen(buf), budget);
        if (take > sizeof(lineBuf) - 4) take = sizeof(lineBuf) - 4;
        memcpy(lineBuf, buf, take);
        memcpy(lineBuf + take, kEllipsis, 3);
        lineBuf[take + 3] = '\0';
        gfx.drawText(fontId, alignedTextX(gfx, ev, lineBuf), ev.rect.y, lineBuf, true);
      } else {
        gfx.drawText(fontId, alignedTextX(gfx, ev, buf), ev.rect.y, buf, true);
      }
      return;
    }
    const size_t total = strlen(buf);
    size_t offset = 0;
    int y = ev.rect.y;
    for (int line = 0; line < maxLines && offset < total; ++line) {
      const bool last = (line == maxLines - 1);
      const char* rest = buf + offset;
      const size_t restLen = total - offset;
      char lineBuf[256];
      size_t take = 0;
      if (last && ev.ellipsis && gfx.getTextWidth(fontId, rest) > maxW) {
        static const char kEllipsis[] = "\xE2\x80\xA6";
        const int ellW = gfx.getTextWidth(fontId, kEllipsis);
        const int budget = maxW > ellW ? maxW - ellW : 0;
        take = fitUtf8Prefix(gfx, fontId, rest, restLen, budget);
        if (take > sizeof(lineBuf) - 4) take = sizeof(lineBuf) - 4;
        memcpy(lineBuf, rest, take);
        memcpy(lineBuf + take, kEllipsis, 3);
        lineBuf[take + 3] = '\0';
      } else {
        take = last ? restLen : fitUtf8Prefix(gfx, fontId, rest, restLen, maxW);
        if (!last && take == 0) {
          take = utf8CharBytes(rest);
          if (take == 0 || take > restLen) break;
        }
        if (last && gfx.getTextWidth(fontId, rest) > maxW) {
          take = fitUtf8Prefix(gfx, fontId, rest, restLen, maxW);
        }
        if (take > sizeof(lineBuf) - 1) take = sizeof(lineBuf) - 1;
        memcpy(lineBuf, rest, take);
        lineBuf[take] = '\0';
      }
      gfx.drawText(fontId, alignedTextX(gfx, ev, lineBuf), y, lineBuf, true);
      offset += take;
      y += lineH;
    }
  }

  // Draw a single 1-bit asset at dst. Black ink only, white = no-op.
  // Asset is 1bpp MSB first, stride bytes per row, 1=black.
  template <typename Gfx>
  static void draw1BitAsset(const Gfx& gfx, const UiSceneAsset& asset, int16_t dstX, int16_t dstY) {
    if (!asset.valid()) return;
    for (uint16_t y = 0; y < asset.height; ++y) {
      for (uint16_t x = 0; x < asset.width; ++x) {
        if (asset.isBlack(x, y)) {
          gfx.drawPixel(static_cast<int>(dstX + x), static_cast<int>(dstY + y), true);
        }
      }
    }
  }

  static bool isInsideRoundedRect(int px, int py, int rx, int ry, int rw, int rh, int r) {
    if (px < rx || px >= rx + rw || py < ry || py >= ry + rh) return false;
    if (r <= 0) return true;
    // Clamp radius to half of smallest dimension
    if (r > rw/2) r = rw/2;
    if (r > rh/2) r = rh/2;
    // Inside the straight edges
    if (px >= rx + r && px < rx + rw - r) return true;
    if (py >= ry + r && py < ry + rh - r) return true;
    // Corner checks
    int cx, cy, dx, dy;
    // top-left
    if (px < rx + r && py < ry + r) {
      cx = rx + r; cy = ry + r;
      dx = cx - px; dy = cy - py;
      return dx*dx + dy*dy <= r*r;
    }
    if (px >= rx + rw - r && py < ry + r) {
      cx = rx + rw - r - 1; cy = ry + r;
      dx = px - cx; dy = cy - py;
      return dx*dx + dy*dy <= r*r;
    }
    if (px < rx + r && py >= ry + rh - r) {
      cx = rx + r; cy = ry + rh - r - 1;
      dx = cx - px; dy = py - cy;
      return dx*dx + dy*dy <= r*r;
    }
    if (px >= rx + rw - r && py >= ry + rh - r) {
      cx = rx + rw - r - 1; cy = ry + rh - r - 1;
      dx = px - cx; dy = py - cy;
      return dx*dx + dy*dy <= r*r;
    }
    return true;
  }

  // Visible placeholder for missing covers (hero 138x191 + mini 92x122).
  // Not empty white: rounded border + diagonal cross + centered book spine.
  template <typename Gfx>
  static void drawCoverPlaceholder(const Gfx& gfx, const UiSceneRuntime::RenderEvent& ev) {
    const int rx = ev.rect.x;
    const int ry = ev.rect.y;
    const int rw = ev.rect.width;
    const int rh = ev.rect.height;
    const int r = ev.radius;
    if (rw <= 0 || rh <= 0) return;
    // Outer border (rounded when radius set)
    if (r > 0) {
      gfx.drawRoundedRect(rx, ry, rw, rh, 1, r, true);
    } else {
      gfx.drawRect(rx, ry, rw, rh, 1, true);
    }
    if (rw < 12 || rh < 12) return;
    // Diagonal cross (inset 3px from border so it doesn't overlap the stroke)
    const int x0 = rx + 3;
    const int y0 = ry + 3;
    const int x1 = rx + rw - 4;
    const int y1 = ry + rh - 4;
    if (x1 > x0 && y1 > y0) {
      gfx.drawLine(x0, y0, x1, y1, true);
      gfx.drawLine(x1, y0, x0, y1, true);
    }
    // Centered book-rect ( ~40% of cover, centered )
    const int bw = rw * 2 / 5;
    const int bh = rh / 4;
    if (bw >= 12 && bh >= 8) {
      const int bx = rx + (rw - bw) / 2;
      const int by = ry + (rh - bh) / 2;
      gfx.drawRect(bx, by, bw, bh, 1, true);
      // Small spine line inside the book rect
      const int spineX = bx + bw / 4;
      gfx.drawLine(spineX, by, spineX, by + bh - 1, true);
    }
  }

  // Cover rendering: aspect-fill (preserve aspect, center-crop), rounded clipping, 1px outer stroke last.
  // Destination rect size is respected; do NOT blit at native asset size.
  // Missing asset still falls back to placeholder elsewhere.
  template <typename Gfx>
  static void drawCoverAsset(const Gfx& gfx, const UiSceneAsset& asset, const UiSceneRuntime::RenderEvent& ev) {
    if (!asset.valid() || ev.rect.width == 0 || ev.rect.height == 0) return;
    const int rx = ev.rect.x;
    const int ry = ev.rect.y;
    const int rw = ev.rect.width;
    const int rh = ev.rect.height;
    const int r = ev.radius;
    // Aspect-fill: scale = max(rw/aw, rh/ah)
    // Use fixed-point 16.16 to keep deterministic
    // For bounded hot path, compute float once (deterministic on host and device with same IEEE? Use integer)
    // Keep simple float for host determinism; on device same.
    float scaleW = static_cast<float>(rw) / static_cast<float>(asset.width);
    float scaleH = static_cast<float>(rh) / static_cast<float>(asset.height);
    float scale = scaleW > scaleH ? scaleW : scaleH;
    if (scale <= 0) return;
    float srcWf = static_cast<float>(rw) / scale;
    float srcHf = static_cast<float>(rh) / scale;
    if (srcWf <= 0 || srcHf <= 0) return;
    // Clamp to asset dims
    if (srcWf > asset.width) srcWf = static_cast<float>(asset.width);
    if (srcHf > asset.height) srcHf = static_cast<float>(asset.height);
    float srcX0 = (static_cast<float>(asset.width) - srcWf) * 0.5f;
    float srcY0 = (static_cast<float>(asset.height) - srcHf) * 0.5f;
    // Draw filled area with clipping
    for (int dy = 0; dy < rh; ++dy) {
      for (int dx = 0; dx < rw; ++dx) {
        const int px = rx + dx;
        const int py = ry + dy;
        if (!isInsideRoundedRect(px, py, rx, ry, rw, rh, r)) continue;
        float fx = srcX0 + (static_cast<float>(dx) + 0.5f) / scale;
        float fy = srcY0 + (static_cast<float>(dy) + 0.5f) / scale;
        int sx = static_cast<int>(fx);
        int sy = static_cast<int>(fy);
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx >= asset.width) sx = asset.width - 1;
        if (sy >= asset.height) sy = asset.height - 1;
        if (asset.isBlack(static_cast<uint16_t>(sx), static_cast<uint16_t>(sy))) {
          gfx.drawPixel(px, py, true);
        }
      }
    }
    // 1px black outer stroke drawn last, on top of fill, clipped to rounded outline
    // Draw outer rounded rect border 1px
    gfx.drawRoundedRect(rx, ry, rw, rh, 1, r, true);
  }

  // Draw M4TH package's ASSET_DATA (type 5) as black-ink overlay.
  // Used for Mofei template and generic bitmap ordered node when source is package.
  // Returns false if no ASSET_DATA.
  template <typename Gfx>
  static bool drawPackageAssetData(const Gfx& gfx, const uint8_t* packageData, size_t packageLen) {
    if (!packageData || packageLen < 32) return false;
    // Find section 5.
    UiScene::SectionInfo info{};
    if (!UiScene::findSection(packageData, packageLen, UiScene::kAssetDataSection, &info)) return false;
    if (info.length != 48000 || info.offset + info.length > packageLen) return false;
    const uint8_t* base = packageData + info.offset;
    // Stream: stride 60, MSB first, 1=black.
    for (int y = 0; y < 800; ++y) {
      for (int x = 0; x < 480; ++x) {
        const size_t byteOff = static_cast<size_t>(y) * 60 + (x >> 3);
        const uint8_t b = pgm_read_byte(base + byteOff);
        if (b & (1u << (7 - (x & 7)))) {
          gfx.drawPixel(x, y, true);
        }
      }
    }
    return true;
  }

  // Ordered scene render. Returns false on malformed package.
  // Exactly honors M4TH v1 forEachCommand order and 4-byte padding.
  template <typename Gfx>
  bool render(const uint8_t* packageData, size_t packageLen,
              const UiSceneRuntime::SceneBindingSource& source,
              const UiSceneAssets& assets,
              const Gfx& gfx) const {
    if (!packageData || packageLen == 0) return false;
    M4FixedRuntimeUiFonts::ensureHubFaces(const_cast<Gfx&>(gfx));

    struct Ctx {
      const Gfx* gfx;
      const UiSceneAssets* assets;
    } ctx{&gfx, &assets};

    auto sinkFn = [](void* user, const UiSceneRuntime::RenderEvent& ev) {
      const Ctx* c = static_cast<const Ctx*>(user);
      const Gfx& gfx = *c->gfx;
      const UiSceneAssets& assets = *c->assets;

      switch (ev.type) {
        case UiScene::kNodeClear: {
          // color 0=white, 1=black (per compile_home_theme)
          if (ev.color == 0) {
            gfx.clearScreen(0xFF);
          } else {
            gfx.fillRect(0, 0, 480, 800, true);
          }
          break;
        }
        case UiScene::kNodeLine: {
          const bool black = ev.color != 0;
          const int w = ev.width ? ev.width : 1;
          if (w <= 1) {
            gfx.drawLine(ev.rect.x, ev.rect.y, ev.x2, ev.y2, black);
          } else {
            gfx.drawLine(ev.rect.x, ev.rect.y, ev.x2, ev.y2, w, black);
          }
          break;
        }
        case UiScene::kNodeRect: {
          const bool fill = ev.fill != 0;
          if (fill) {
            gfx.fillRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, true);
          } else {
            const int stroke = ev.width ? ev.width : 1;
            gfx.drawRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, stroke, true);
          }
          break;
        }
        case UiScene::kNodeRoundRect: {
          const bool fill = ev.fill != 0;
          const int r = ev.radius;
          const int stroke = ev.width ? ev.width : 1;
          if (fill) {
            gfx.drawRoundedRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, 1, r, true);
          } else {
            gfx.drawRoundedRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, stroke, r, true);
          }
          break;
        }
        case UiScene::kNodeText: {
          char buf[256]{};
          size_t n = ev.text.size < 255 ? ev.text.size : 255;
          for (size_t i = 0; i < n; ++i) buf[i] = static_cast<char>(ev.text.readByte(static_cast<uint16_t>(i)));
          buf[n] = '\0';
          if (n > 0) drawSceneText(gfx, ev, buf);
          break;
        }
        case UiScene::kNodeCover: {
          if (ev.assetBinding != kInvalidBindingId) {
            const UiSceneAsset* a = assets.get(assetKey(ev));
            if (a && a->valid()) {
              drawCoverAsset(gfx, *a, ev);
            } else {
              drawCoverPlaceholder(gfx, ev);
            }
          } else if (ev.text.size > 0) {
            const UiSceneAsset* a = assets.get(assetKey(ev));
            if (a && a->valid()) {
              drawCoverAsset(gfx, *a, ev);
            } else {
              drawCoverPlaceholder(gfx, ev);
            }
          } else {
            drawCoverPlaceholder(gfx, ev);
          }
          break;
        }
        case UiScene::kNodeProgress: {
          // Outline
          const int r = ev.radius;
          if (r > 0) {
            gfx.drawRoundedRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, 1, r, true);
          } else {
            gfx.drawRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, 1, true);
          }
          int pct = ev.value;
          if (pct < 0) pct = 0; if (pct > 100) pct = 100;
          if (pct > 0 && ev.rect.width > 4 && ev.rect.height > 4) {
            const int innerW = ev.rect.width - 4;
            const int fillW = (innerW * pct) / 100;
            if (fillW > 0) {
              if (r > 0) {
                gfx.fillRect(ev.rect.x + 2, ev.rect.y + 2, fillW, ev.rect.height - 4, true);
              } else {
                gfx.fillRect(ev.rect.x + 2, ev.rect.y + 2, fillW, ev.rect.height - 4, true);
              }
            }
          }
          break;
        }
        case UiScene::kNodeBattery: {
          // Battery: small outline + filled segments based on value 0-100
          const int pct = ev.value < 0 ? 0 : (ev.value > 100 ? 100 : ev.value);
          const int w = ev.rect.width;
          const int h = ev.rect.height;
          // Outline
          gfx.drawRect(ev.rect.x, ev.rect.y, w, h, 1, true);
          // Terminal nub (2px)
          gfx.fillRect(ev.rect.x + w, ev.rect.y + h/4, 2, h/2, true);
          // Fill segments (4 segments)
          int segs = 0;
          if (pct > 0) segs = 1;
          if (pct > 25) segs = 2;
          if (pct > 50) segs = 3;
          if (pct > 75) segs = 4;
          if (segs > 0) {
            const int innerX = ev.rect.x + 2;
            const int innerY = ev.rect.y + 2;
            const int innerW = w - 4;
            const int innerH = h - 4;
            const int segW = (innerW - 3) / 4;
            for (int i=0;i<segs;++i) {
              gfx.fillRect(innerX + i*(segW+1), innerY, segW, innerH, true);
            }
          }
          break;
        }
        case UiScene::kNodeIcon: {
          if (ev.assetBinding != kInvalidBindingId) {
            const UiSceneAsset* a = assets.get(assetKey(ev));
            if (a && a->valid()) {
              draw1BitAsset(gfx, *a, ev.rect.x, ev.rect.y);
            } else {
              gfx.drawRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, 1, true);
            }
          } else {
            // A named icon without a resolved asset has no safe bitmap buffer.
            gfx.drawRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, 1, true);
          }
          break;
        }
        case UiScene::kNodeBitmap: {
          // Ordered bitmap node. Prefer UiSceneAssets[0] if provided (covers template asset),
          // else fallback to package ASSET_DATA if available.
          // The black-ink semantics are identical: 1=black, 0=transparent.
          if (assets.count > 0) {
            const UiSceneAsset* a = assets.get(0);
            if (a && a->valid()) {
              draw1BitAsset(gfx, *a, ev.rect.x, ev.rect.y);
              break;
            }
          }
          // No explicit asset: if package has ASSET_DATA, the bitmap node is
          // conceptually that full-screen 1bpp asset positioned per rect.
          // For ordered semantics, we draw the package asset at its rect.
          // If rect is empty/zero, treat as full-screen overlay.
          // For now, if no asset, draw a placeholder rect to preserve order.
          // Check if package ASSET_DATA exists and rect matches screen; if so, use it.
          // Since we don't have package pointer here, draw placeholder rect.
          // The package-level drawPackageAssetData is available for full-screen template case.
          // For scene bitmap, we use assets[0] above.
          gfx.drawRect(ev.rect.x, ev.rect.y, ev.rect.width, ev.rect.height, 1, true);
          break;
        }
        default: break;
      }
    };

    UiSceneRuntime::SceneRenderSink sink{const_cast<Ctx*>(&ctx), [](void* u, const UiSceneRuntime::RenderEvent& ev){
      auto* c = static_cast<Ctx*>(u);
      // Reconstruct the lambda capture manually: we need to call the same switch.
      // To avoid duplication, we call a helper function that takes Gfx&.
      // Inline the handling here by re-invoking the outer lambda's logic via function pointer.
      // Simplify: direct switch (duplicate but okay for now)
      (void)c; (void)ev;
    }};

    // Actual dispatch via UiSceneRuntime
    bool ok = UiSceneRuntime::renderScene(packageData, packageLen, source,
      UiSceneRuntime::SceneRenderSink{&ctx, [](void* u, const UiSceneRuntime::RenderEvent& ev){
        Ctx* c = static_cast<Ctx*>(u);
        // Reuse the same switch as above — we need to duplicate logic to avoid capturing issues.
        const Gfx& gfx = *c->gfx;
        const UiSceneAssets& assets = *c->assets;
        switch(ev.type){
          case UiScene::kNodeClear: if(ev.color==0) gfx.clearScreen(0xFF); else gfx.fillRect(0,0,480,800,true); break;
          case UiScene::kNodeLine: {
            const bool black = ev.color!=0; const int w=ev.width?ev.width:1;
            if(w<=1) gfx.drawLine(ev.rect.x,ev.rect.y,ev.x2,ev.y2,black); else gfx.drawLine(ev.rect.x,ev.rect.y,ev.x2,ev.y2,w,black);
            break;
          }
          case UiScene::kNodeRect: {
            const bool fill=ev.fill!=0; if(fill) gfx.fillRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,true);
            else { int s=ev.width?ev.width:1; gfx.drawRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,s,true); }
            break;
          }
          case UiScene::kNodeRoundRect: {
            const bool fill=ev.fill!=0; int r=ev.radius; int s=ev.width?ev.width:1;
            if(fill) gfx.drawRoundedRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,1,r,true);
            else gfx.drawRoundedRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,s,r,true);
            break;
          }
          case UiScene::kNodeText: {
            char buf[256]={}; size_t n=ev.text.size<255?ev.text.size:255;
            for(size_t i=0;i<n;++i) buf[i]=(char)ev.text.readByte((uint16_t)i);
            buf[n]='\0'; if(n>0) GfxSceneRenderer::drawSceneText(gfx,ev,buf);
            break;
          }
          case UiScene::kNodeCover: {
            if(ev.assetBinding!=kInvalidBindingId){ auto a=assets.get(GfxSceneRenderer::assetKey(ev)); if(a&&a->valid()) GfxSceneRenderer::drawCoverAsset(gfx,*a,ev); else GfxSceneRenderer::drawCoverPlaceholder(gfx,ev);}
            else if(ev.text.size>0){ auto a=assets.get(GfxSceneRenderer::assetKey(ev)); if(a&&a->valid()) GfxSceneRenderer::drawCoverAsset(gfx,*a,ev); else GfxSceneRenderer::drawCoverPlaceholder(gfx,ev);}
            else GfxSceneRenderer::drawCoverPlaceholder(gfx,ev);
            break;
          }
          case UiScene::kNodeProgress: {
            int r=ev.radius; if(r>0) gfx.drawRoundedRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,1,r,true); else gfx.drawRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,1,true);
            int pct=ev.value; if(pct<0)pct=0; if(pct>100)pct=100;
            if(pct>0 && ev.rect.width>4 && ev.rect.height>4){ int innerW=ev.rect.width-4; int fillW=(innerW*pct)/100; if(fillW>0){ gfx.fillRect(ev.rect.x+2,ev.rect.y+2,fillW,ev.rect.height-4,true); } }
            break;
          }
          case UiScene::kNodeBattery: {
            int pct=ev.value; if(pct<0)pct=0; if(pct>100)pct=100;
            int w=ev.rect.width, h=ev.rect.height;
            gfx.drawRect(ev.rect.x,ev.rect.y,w,h,1,true);
            gfx.fillRect(ev.rect.x+w,ev.rect.y+h/4,2,h/2,true);
            int segs=0; if(pct>0)segs=1; if(pct>25)segs=2; if(pct>50)segs=3; if(pct>75)segs=4;
            if(segs>0){ int ix=ev.rect.x+2, iy=ev.rect.y+2, iw=w-4, ih=h-4, sw=(iw-3)/4; for(int i=0;i<segs;++i) gfx.fillRect(ix+i*(sw+1),iy,sw,ih,true); }
            break;
          }
          case UiScene::kNodeIcon: {
            if(ev.assetBinding!=kInvalidBindingId){ auto a=assets.get(GfxSceneRenderer::assetKey(ev)); if(a&&a->valid()) GfxSceneRenderer::draw1BitAsset(gfx,*a,ev.rect.x,ev.rect.y); else gfx.drawRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,1,true); }
            else { gfx.drawRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,1,true); }
            break;
          }
          case UiScene::kNodeBitmap: {
            if(assets.count>0){ auto a=assets.get(0); if(a&&a->valid()){ GfxSceneRenderer::draw1BitAsset(gfx,*a,ev.rect.x,ev.rect.y); break; } }
            gfx.drawRect(ev.rect.x,ev.rect.y,ev.rect.width,ev.rect.height,1,true);
            break;
          }
          default: break;
        }
      }});
    return ok;
  }
};

} // namespace UiScene

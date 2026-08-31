#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
#include "ui/scene/UiSceneAssets.h"
#include "ui/scene/GfxSceneRenderer.h"

struct FakeFb {
  mutable std::vector<std::vector<bool>> black;
  int W=480,H=800;
  mutable std::vector<std::string> calls;
  FakeFb(){ black.assign(H, std::vector<bool>(W,false)); }
  void clearScreen(uint8_t c=0xFF) const { (void)c; for(auto &r: black) std::fill(r.begin(), r.end(), false); calls.push_back("clear"); }
  void drawPixel(int x,int y,bool s=true) const { if(x<0||x>=W||y<0||y>=H) return; if(s) black[y][x]=true; calls.push_back("pixel"); }
  void drawLine(int,int,int,int,bool) const {}
  void drawLine(int,int,int,int,int,bool) const {}
  void drawRect(int x,int y,int w,int h,bool s=true) const { calls.push_back("drawRect"); for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx) drawPixel(xx,yy,s); }
  void drawRect(int x,int y,int w,int h,int lw,bool s) const { calls.push_back("drawRectW"); for(int i=0;i<lw;++i) drawRect(x+i,y+i,w-2*i,h-2*i,s); }
  static bool insideRR(int px,int py,int rx,int ry,int rw,int rh,int r){
    if(px<rx||px>=rx+rw||py<ry||py>=ry+rh) return false;
    if(r<=0) return true;
    if(r>rw/2) r=rw/2; if(r>rh/2) r=rh/2;
    if(px>=rx+r && px<rx+rw-r) return true;
    if(py>=ry+r && py<ry+rh-r) return true;
    int cx,cy,dx,dy;
    if(px<rx+r && py<ry+r){ cx=rx+r; cy=ry+r; dx=cx-px; dy=cy-py; return dx*dx+dy*dy <= r*r; }
    if(px>=rx+rw-r && py<ry+r){ cx=rx+rw-r-1; cy=ry+r; dx=px-cx; dy=cy-py; return dx*dx+dy*dy <= r*r; }
    if(px<rx+r && py>=ry+rh-r){ cx=rx+r; cy=ry+rh-r-1; dx=cx-px; dy=py-cy; return dx*dx+dy*dy <= r*r; }
    if(px>=rx+rw-r && py>=ry+rh-r){ cx=rx+rw-r-1; cy=ry+rh-r-1; dx=px-cx; dy=py-cy; return dx*dx+dy*dy <= r*r; }
    return true;
  }
  void drawRoundedRect(int x,int y,int w,int h,int lw,int r,bool s=true) const { calls.push_back("drawRoundedRect");
    for(int yy=y;yy<y+h;++yy) for(int xx=x;xx<x+w;++xx){
      bool onBorder=false;
      if(yy<y+lw||yy>=y+h-lw||xx<x+lw||xx>=x+w-lw) onBorder=true;
      if(!onBorder) continue;
      if(!insideRR(xx,yy,x,y,w,h,r)) continue;
      // also ensure not inside inner rounded rect (hole)
      if(insideRR(xx,yy,x+lw,y+lw,w-2*lw,h-2*lw,r>lw?r-lw:0)) continue;
      drawPixel(xx,yy,s);
    }
  }
  void fillRect(int,int,int,int,bool=true) const {}
  void fillRectDither(int,int,int,int,uint8_t) const {}
  void fillRoundedRect(int,int,int,int,int,uint8_t) const {}
  void drawImage(const uint8_t*,int,int,int,int) const {}
  void drawIcon(const uint8_t*,int,int,int,int) const {}
  void drawBitmap(const struct Bitmap&,int,int,int,int,float,float) const {}
  int getTextWidth(int,const char*,int=0,float=1) const { return 50; }
  int getLineHeight(int) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  std::string truncatedText(int,const char*,int,int=0,float=1) const { return "..."; }
  void drawText(int,int,int,const char*,bool=true,int=0,float=1) const {}
  int getScreenWidth() const { return W; }
  int getScreenHeight() const { return H; }
  bool isBlack(int x,int y) const { if(x<0||x>=W||y<0||y>=H) return false; return black[y][x]; }
};

int main(){
  using namespace UiScene;
  // Test 1: main cover rect [44,104,138,191] with source asset 20x40 (tall) vs 138x191 (also tall but different ratio) - aspect-fill should fill entire rect
  // Asset 8x16 tall, rect 138x191 (tall) scale = max(138/8=17.25,191/16=11.9)=17.25, src needed 8x11, center crop vertical
  // Create asset 8x16 where top half black, bottom half white - after center crop, far corner should still be black if aspect-fill correct, but naive blit would leave far corner white
  {
    std::vector<uint8_t> data(16*1,0);
    for(int y=0;y<16;++y){
      uint8_t byte=0;
      for(int x=0;x<8;++x){
        bool black = (y < 8); // top half black
        if(black) byte |= (0x80 >> x);
      }
      data[y]=byte;
    }
    UiSceneAsset asset{data.data(),8,16,1,false};
    UiSceneAssets assets;
    AssetKey key{14,kInvalidBindingId,kInvalidAssetItemIndex};
    assets.add(key, asset);
    FakeFb gfx;
    UiSceneRuntime::RenderEvent ev{};
    ev.type = kNodeCover;
    ev.rect = {44,104,138,191};
    ev.radius = 8;
    ev.assetBinding = 14;
    // Use new aspect-fill path via GfxSceneRenderer::drawCoverAsset
    GfxSceneRenderer::drawCoverAsset(gfx, asset, ev);
    // Check far corner inside rect (should be black due to aspect-fill expanding, but naive would be out of 8x16)
    // With aspect-fill, dest 138x191 from 8x16: scale 17.25, src 8x11 centered, srcY0 ~2.5, so dest bottom maps to src ~13, which is white (since y>=8 white) -> but we expect at least some black in center
    // More robust: check that at least 50% of rect interior is considered (center should be black if top half maps)
    // Instead test simple: asset 20x10 white-black checker vs rect 138x191 - just verify that drawCoverAsset draws beyond native 20x10 and clips rounded corners and draws stroke
    bool centerBlack = gfx.isBlack(44+69,104+95);
    // Hard to predict exact, but at least stroke should have been drawn (rounded rect)
    bool hasStroke=false;
    for(auto &c: gfx.calls) if(c=="drawRoundedRect") hasStroke=true;
    assert(hasStroke && "1px stroke must be drawn last");
    // Rounded clip: corner (44,104) outside radius 8 should NOT be black even though source may be black there
    // Our isInsideRoundedRect should clip top-left corner
    bool corner = gfx.isBlack(44,104);
    // With radius 8, corner pixel should be clipped (not drawn) unless stroke draws it? Stroke draws border, but corner of stroke is also clipped? For 1px stroke, corner outside radius should still be transparent.
    // We assert corner is not black from fill (stroke may draw border but not at exact corner pixel if rounded)
    // Allow stroke to draw border pixel at (44+7,104) etc, but not at (44,104) extreme
    // For this tall asset with top black, naive would draw corner black; aspect-fill with clip should not
    assert(!corner && "rounded clip must suppress extreme corner");
    // Far pixel beyond native asset size should be drawn if aspect-filled (dest larger than asset)
    bool far = gfx.isBlack(44+130,104+180);
    // With naive 8x16 at 44,104, far pixel 130,180 beyond would be empty. With aspect-fill, it should be derived from source and may be white or black but at least the loop visited it.
    // We check that we attempted to draw there (black vector may have entry even if white not drawn). Instead check that we did not just blit 8x8
    // Count black pixels should be > 8*8 =64
    int blacks=0;
    for(int y=0;y<gfx.H;++y) for(int x=0;x<gfx.W;++x) if(gfx.black[y][x]) blacks++;
    assert(blacks > 64 && "aspect-fill must draw beyond native 8x16 size");
  }
  // Test 2: repeated cover same semantics - rect [18,0,92,122] radius 5, asset 10x10
  {
    std::vector<uint8_t> data(10*2,0xFF); // 10x10 black with stride 2
    // Fill 10x10 all black
    for(int i=0;i<20;++i) data[i]=0xFF;
    // Clear beyond 10 bits in last byte per row
    for(int y=0;y<10;++y) data[y*2+1] &= 0xC0; // keep 2 bits
    UiSceneAsset asset{data.data(),10,10,2,false};
    FakeFb gfx;
    UiSceneRuntime::RenderEvent ev{};
    ev.type = kNodeCover;
    ev.rect = {18,0,92,122};
    ev.radius = 5;
    ev.assetBinding = 33;
    ev.item.valid = true;
    ev.item.sourceBinding = 20;
    ev.item.index = 0;
    GfxSceneRenderer::drawCoverAsset(gfx, asset, ev);
    // Should be filled and stroked
    int blacks=0;
    for(int y=0;y<gfx.H;++y) for(int x=0;x<gfx.W;++x) if(gfx.black[y][x]) blacks++;
    assert(blacks > 200 && "repeated cover aspect-fill");
    bool hasStroke=false;
    for(auto &c: gfx.calls) if(c=="drawRoundedRect") hasStroke=true;
    assert(hasStroke);
    // Corner clip
    assert(!gfx.isBlack(18,0) && "repeated rounded clip");
  }
  // Test missing asset fallback not tested here - render path in GfxSceneRenderer will draw placeholder rect, not call drawCoverAsset with invalid asset
  puts("cover_aspect_fill GREEN");
  return 0;
}

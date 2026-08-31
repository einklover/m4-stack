#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ui/scene/UiSceneAssets.h"
#include "ui/scene/GfxSceneRenderer.h"
#include "ui/scene/UiSceneRuntime.h"
#include "ui/scene/UiScenePackage.h"
#include "generated/murphy_default_m4theme.h"
#include "fontIds.h"

// Spy Gfx — records calls, tracks forbidden SD/Bitmap file I/O.
struct SpyGfx {
  struct Call { std::string name; int x=0,y=0,w=0,h=0; int x2=0,y2=0; int r=0, stroke=0; std::string text; int font=0; bool fill=false; uint8_t color=0; };
  mutable std::vector<Call> calls;
  mutable int clearCount=0;
  mutable int drawPixelCount=0;
  mutable int forbiddenCalls=0;

  void clearScreen(uint8_t c=0xFF) const { calls.push_back({"clearScreen",0,0,0,0,0,0,0,0,"",0,false,c}); clearCount++; }
  void drawPixel(int x,int y,bool s=true) const { calls.push_back({"drawPixel",x,y,1,1,0,0,0,0,"",0,s,0}); drawPixelCount++; }
  void drawLine(int x1,int y1,int x2,int y2,bool s=true) const { calls.push_back({"drawLine",x1,y1,x2,y2,0,0,0,1,"",0,s,0}); }
  void drawLine(int x1,int y1,int x2,int y2,int w,bool s) const { calls.push_back({"drawLineW",x1,y1,x2,y2,0,0,0,w,"",0,s,0}); }
  void drawRect(int x,int y,int w,int h,bool s=true) const { calls.push_back({"drawRect",x,y,w,h,0,0,0,1,"",0,s,0}); }
  void drawRect(int x,int y,int w,int h,int lw,bool s) const { calls.push_back({"drawRectW",x,y,w,h,0,0,0,lw,"",0,s,0}); }
  void drawRoundedRect(int x,int y,int w,int h,int lw,int r,bool s=true) const { calls.push_back({"drawRoundedRect",x,y,w,h,0,0,r,lw,"",0,s,0}); }
  void fillRect(int x,int y,int w,int h,bool s=true) const { calls.push_back({"fillRect",x,y,w,h,0,0,0,0,"",0,s,0}); }
  void fillRectDither(int x,int y,int w,int h,uint8_t c) const { calls.push_back({"fillRectDither",x,y,w,h,0,0,0,0,"",0,false,c}); }
  void fillRoundedRect(int x,int y,int w,int h,int r,uint8_t c) const { calls.push_back({"fillRoundedRect",x,y,w,h,0,0,r,0,"",0,false,c}); }
  void drawImage(const uint8_t*,int x,int y,int w,int h) const { calls.push_back({"drawImage",x,y,w,h,0,0,0,0,"",0,true,0}); }
  void drawIcon(const uint8_t*,int x,int y,int w,int h) const { calls.push_back({"drawIcon",x,y,w,h,0,0,0,0,"",0,true,0}); }
  void drawBitmap(const struct Bitmap&,int,int,int,int,float,float) const { forbiddenCalls++; }
  int getTextWidth(int, const char*, int=0,float=1) const { return 50; }
  int getLineHeight(int) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  std::string truncatedText(int, const char*, int, int=0,float=1) const { return std::string("..."); }
  void drawText(int fid,int x,int y,const char* t,bool b=true,int s=0,float sc=1) const { calls.push_back({"drawText",x,y,0,0,0,0,0,0,t?t:"",fid,b,0}); }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
};

using namespace UiScene;
using namespace UiSceneRuntime;

// Helpers to build a valid M4TH package with ordered nodes (mirrors test_ui_scene_runtime builder)
struct SceneBuilder {
  uint8_t scene[2048]{};
  uint8_t package[2304]{};
  size_t sceneSize = 8;
  uint16_t commandCount = 0;
  static void put16(uint8_t* p, uint16_t v){ p[0]=v&0xFF; p[1]=v>>8; }
  static void put32(uint8_t* p, uint32_t v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
  static size_t cmd(uint8_t* out,uint8_t t,uint8_t f,const uint8_t* pay,size_t n){
    out[0]=t; out[1]=f; put16(out+2,(uint16_t)n);
    if(n) memcpy(out+4,pay,n);
    size_t tot=(4+n+3)&~3; memset(out+4+n,0,tot-4-n); return tot;
  }
  void add(uint8_t t,uint8_t f,const uint8_t* pay,size_t n){ size_t w=cmd(scene+sceneSize,t,f,pay,n); sceneSize+=w; ++commandCount; }
  static size_t pref(uint8_t* out,uint8_t f,BindingId vis,ActionId act,BindingId arg=kInvalidBindingId){
    size_t s=0; if(f&kFlagVisibleIf) out[s++]=vis; if(f&kFlagAction){ out[s++]=act; out[s++]=arg==kInvalidBindingId?0:1; if(arg!=kInvalidBindingId) out[s++]=arg; } return s;
  }
  void addClear(uint8_t color=0){ uint8_t p[1]={color}; add(kNodeClear,0,p,1); }
  void addText(const char* lit,BindingId b=kInvalidBindingId,uint8_t f=0,BindingId vis=kInvalidBindingId,ActionId act=kInvalidActionId,BindingId arg=kInvalidBindingId,uint16_t x=0,uint16_t y=0,uint8_t font=16,uint16_t width=180,uint8_t align=0,uint16_t height=24){
    uint8_t pay[128]={}; size_t s=pref(pay,f,vis,act,arg);
    put16(pay+s,x); put16(pay+s+2,y); put16(pay+s+4,width); put16(pay+s+6,height);
    pay[s+8]=font; pay[s+9]=0; pay[s+10]=align; pay[s+11]=1;
    pay[s+12]= b==kInvalidBindingId?0:1; pay[s+13]= b==kInvalidBindingId?0:b;
    size_t litN=b==kInvalidBindingId?strlen(lit):0; put16(pay+s+14,(uint16_t)litN);
    if(litN) memcpy(pay+s+16,lit,litN); add(kNodeText,f,pay,s+16+litN);
  }
  void addBitmap(uint16_t x=10,uint16_t y=20,uint16_t w=30,uint16_t h=40){
    uint8_t pay[32]={}; put16(pay,x); put16(pay+2,y); put16(pay+4,w); put16(pay+6,h); put16(pay+8,1); pay[10]='a'; add(kNodeBitmap,0,pay,11);
  }
  void addLine(uint16_t x=0,uint16_t y=50,uint16_t x2=480,uint16_t y2=50){ uint8_t pay[10]={}; put16(pay,x); put16(pay+2,y); put16(pay+4,x2); put16(pay+6,y2); pay[8]=1; pay[9]=1; add(kNodeLine,0,pay,10); }
  void addRect(uint8_t type=kNodeRect,uint16_t x=0,uint16_t y=0,BindingId b=kInvalidBindingId,uint8_t f=0,BindingId vis=kInvalidBindingId){
    uint8_t pay[32]={}; size_t s=pref(pay,f,vis,kInvalidActionId);
    put16(pay+s,x); put16(pay+s+2,y); put16(pay+s+4,80); put16(pay+s+6,60);
    if(type==kNodeCover){ put16(pay+s+8,4); pay[s+10]=1; pay[s+11]=b; add(type,f,pay,s+12); }
    else if(type==kNodeProgress){ put16(pay+s+8,4); pay[s+10]=b; add(type,f,pay,s+11); }
    else if(type==kNodeBattery){ pay[s+8]=b; add(type,f,pay,s+9); }
    else { // rect/round_rect
      if(type==kNodeRoundRect){ put16(pay+s+8,6); pay[s+10]=1; pay[s+11]=0; add(type,f,pay,s+12); }
      else { pay[s+8]=1; pay[s+9]=0; add(type,f,pay,s+10); }
    }
  }
  void addRepeat(BindingId src,uint8_t lim,uint16_t x,uint16_t y,uint16_t iw,uint16_t ih,uint16_t gap,const uint8_t* ch,size_t chS,uint16_t cc){
    uint8_t pay[512]={}; pay[0]=src; pay[1]=lim; put16(pay+2,x); put16(pay+4,y); put16(pay+6,iw); put16(pay+8,ih); put16(pay+10,gap); pay[12]=0; pay[13]=0; put16(pay+14,cc); memcpy(pay+16,ch,chS); add(kNodeRepeat,0,pay,16+chS);
  }
  size_t finish(uint8_t* out){
    put16(scene,1); put16(scene+2,commandCount);
    memset(out,0,2304);
    put32(out,kM4thMagic); put16(out+4,kM4thVersion); put16(out+6,kM4thHeaderSize);
    put16(out+12,kScreenWidth); put16(out+14,kScreenHeight); put16(out+16,1);
    put32(out+32,kSceneSection); put32(out+40,80); put32(out+44,(uint32_t)sceneSize); put32(out+48,commandCount);
    memcpy(out+80,scene,sceneSize); put32(out+8,80+(uint32_t)sceneSize);
    return 80+sceneSize;
  }
};

static bool resolveInt(const void*, BindingId b, const SceneItemContext*, ResolvedValue* o){
  if(b==1){ o->kind=ValueKind::Int; o->number=73; return true; }
  if(b==15){ o->kind=ValueKind::Int; o->number=42; return true; }
  return false;
}

void testSceneFontIdRoutesToRuntimeFontId(){
  SceneBuilder b;
  b.addText("scene text",kInvalidBindingId,0,kInvalidBindingId,kInvalidActionId,kInvalidBindingId,10,20,12);
  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SceneBindingSource src{nullptr, [](const void*,BindingId, const SceneItemContext*, ResolvedValue*)->bool{return false;}, nullptr};
  SpyGfx gfx; UiSceneAssets assets; GfxSceneRenderer r;
  assert(r.render(pkg,len,src,assets,gfx));
  assert(gfx.calls.size()==1);
  assert(gfx.calls[0].name=="drawText");
  assert(gfx.calls[0].font==UI_12_FONT_ID);
  assert(GfxSceneRenderer::runtimeFontId(14)==NOTOSANS_14_FONT_ID);
  assert(GfxSceneRenderer::runtimeFontId(15)==NOTOSANS_14_FONT_ID);
  assert(GfxSceneRenderer::runtimeFontId(16)==NOTOSANS_16_FONT_ID);
  assert(GfxSceneRenderer::runtimeFontId(19)==NOTOSANS_18_FONT_ID);
}

void testTextAlignmentUsesMeasuredWidthWithinRect(){
  SceneBuilder b;
  b.addText("right",kInvalidBindingId,0,kInvalidBindingId,kInvalidActionId,
            kInvalidBindingId,380,20,14,70,2);
  b.addText("center",kInvalidBindingId,0,kInvalidBindingId,kInvalidActionId,
            kInvalidBindingId,380,50,14,70,1);
  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SceneBindingSource src{nullptr, [](const void*,BindingId, const SceneItemContext*, ResolvedValue*)->bool{return false;}, nullptr};
  SpyGfx gfx; UiSceneAssets assets;
  assert(GfxSceneRenderer{}.render(pkg,len,src,assets,gfx));
  assert(gfx.calls.size()==2);
  assert(gfx.calls[0].name=="drawText" && gfx.calls[0].x==400);
  assert(gfx.calls[1].name=="drawText" && gfx.calls[1].x==390);
}
static bool resolveCoverAsset(const void*, BindingId b, const SceneItemContext* item, ResolvedValue* o){
  if(b==33 && item && item->valid){ o->kind=ValueKind::Asset; o->assetIndex= item->index % 2; return true; }
  if(b==14){ o->kind=ValueKind::Asset; o->assetIndex=0; return true; }
  if(b==1){ o->kind=ValueKind::Int; o->number=73; return true; }
  return false;
}
static uint8_t count3(const void*, BindingId s){ return s==20?3:0; }

void testAssetsRequireExactBindingAndItemContext(){
  UiSceneAssets assets;
  static const uint8_t currentPixels[1]={0x80};
  static const uint8_t recentPixels[1]={0x40};
  static const uint8_t appPixels[1]={0x20};
  const UiSceneAsset current{currentPixels,1,1,1,false};
  const UiSceneAsset recent{recentPixels,1,1,1,false};
  const UiSceneAsset app{appPixels,1,1,1,false};
  const AssetKey currentKey{14,kInvalidBindingId,kInvalidAssetItemIndex};
  const AssetKey recent0Key{33,20,0};
  const AssetKey app0Key{34,21,0};
  assert(assets.add(currentKey,current));
  assert(assets.add(recent0Key,recent));
  assert(assets.add(app0Key,app));
  assert(assets.get(currentKey)->pixels==currentPixels);
  assert(assets.get(recent0Key)->pixels==recentPixels);
  assert(assets.get(app0Key)->pixels==appPixels);
  assert(assets.get(AssetKey{33,21,0})==nullptr);
  assert(assets.get(AssetKey{34,20,0})==nullptr);
  assert(assets.get(AssetKey{33,20,1})==nullptr);

  RenderEvent currentEvent{};
  currentEvent.assetBinding=14;
  RenderEvent recentEvent{};
  recentEvent.assetBinding=33;
  recentEvent.item={true,20,0,1};
  RenderEvent appEvent{};
  appEvent.assetBinding=34;
  appEvent.item={true,21,0,1};
  assert(assets.get(GfxSceneRenderer::assetKey(currentEvent))->pixels==currentPixels);
  assert(assets.get(GfxSceneRenderer::assetKey(recentEvent))->pixels==recentPixels);
  assert(assets.get(GfxSceneRenderer::assetKey(appEvent))->pixels==appPixels);
  appEvent.assetBinding=33;
  assert(assets.get(GfxSceneRenderer::assetKey(appEvent))==nullptr);
}

void testOrderedBitmapSemantics(){
  // clear -> text -> bitmap -> line -> text  must stay ordered
  SceneBuilder b;
  b.addClear(0);
  b.addText("A",kInvalidBindingId,0,kInvalidBindingId,kInvalidActionId,kInvalidBindingId,10,10);
  b.addBitmap(10,20,30,40);
  b.addLine(0,50,480,50);
  b.addText("B",kInvalidBindingId,0,kInvalidBindingId,kInvalidActionId,kInvalidBindingId,10,100);
  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SpyGfx gfx; UiSceneAssets assets;
  // Provide asset for bitmap
  static const uint8_t bmp[8]={0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00};
  UiSceneAsset a{}; a.pixels=bmp; a.width=8; a.height=8; a.stride=1; assets.add(a);
  SceneBindingSource src{nullptr, [](const void*,BindingId, const SceneItemContext*, ResolvedValue*)->bool{return false;}, nullptr};
  GfxSceneRenderer r;
  bool ok=r.render(pkg,len,src,assets,gfx);
  assert(ok);
  assert(gfx.forbiddenCalls==0);
  // Find order: clearScreen, drawText A, drawPixel/drawRect for bitmap, drawLine, drawText B
  // Spy records drawPixel for bitmap's black bits, and drawRect for fallback etc.
  // We at least verify that first text appears before line and second text after line.
  size_t idxA=999, idxBitmap=999, idxLine=999, idxB=999;
  for(size_t i=0;i<gfx.calls.size();++i){
    auto &c=gfx.calls[i];
    if(c.name=="drawText" && c.text=="A" && idxA==999) idxA=i;
    if((c.name=="drawPixel" || c.name=="drawRect"||c.name=="drawRectW") && idxBitmap==999 && i>idxA) idxBitmap=i;
    if(c.name=="drawLine" || c.name=="drawLineW") idxLine=i;
    if(c.name=="drawText" && c.text=="B") idxB=i;
  }
  assert(idxA < idxBitmap);
  assert(idxBitmap < idxLine);
  assert(idxLine < idxB);
}

void testBlackInkNoOpWhite(){
  SpyGfx gfx;
  UiSceneAssets assets;
  // 8x2 asset: row0 = 0b11111111 (8 black), row1 = 0b00000000 (0 black) => exactly 8 pixels
  static const uint8_t pix[2]={0xFF,0x00};
  UiSceneAsset a{}; a.pixels=pix; a.width=8; a.height=2; a.stride=1; assets.add(a);
  // Single bitmap node at 10,20
  SceneBuilder b; b.addBitmap(10,20,8,2);
  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SceneBindingSource src{nullptr, [](const void*,BindingId, const SceneItemContext*, ResolvedValue*)->bool{return false;}, nullptr};
  GfxSceneRenderer r; bool ok=r.render(pkg,len,src,assets,gfx); assert(ok);
  // Black-ink: only 8 drawPixel calls, not 16. White row is no-op.
  assert(gfx.drawPixelCount==8);
  // Verify that all drawn pixels are at y=20 (first row) not y=21
  for(auto &c: gfx.calls) if(c.name=="drawPixel") assert(c.y==20);
  // Now test 8x2 all white -> 0 pixels
  SpyGfx gfx2; UiSceneAssets assets2;
  static const uint8_t pixWhite[2]={0x00,0x00};
  UiSceneAsset a2{}; a2.pixels=pixWhite; a2.width=8; a2.height=2; a2.stride=1; assets2.add(a2);
  gfx2.calls.clear(); gfx2.drawPixelCount=0;
  ok=r.render(pkg,len,src,assets2,gfx2); assert(ok);
  assert(gfx2.drawPixelCount==0);
}

void testCoverUsesCachedAssetOnly(){
  SpyGfx gfx;
  UiSceneAssets assets;
  // Two cover assets for repeat indices 0 and 1 (8x8 border vs solid)
  static const uint8_t pix0[8]={0xFF,0x81,0x81,0x81,0x81,0x81,0xFF,0x00};
  static const uint8_t pix1[8]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  UiSceneAsset a0{}; a0.pixels=pix0; a0.width=8; a0.height=8; a0.stride=1; assets.add(AssetKey{33,20,0},a0);
  UiSceneAsset a1{}; a1.pixels=pix1; a1.width=8; a1.height=8; a1.stride=1; assets.add(AssetKey{33,20,1},a1);
  // Build a repeat of 2 covers
  SceneBuilder b;
  uint8_t children[256]={};
  // child: cover bound to 33
  uint8_t pay[16]={}; pay[0]=0; pay[2]=0; pay[4]=80; pay[6]=80; pay[8]=4&0xFF; pay[10]=1; pay[11]=33;
  size_t sz=SceneBuilder::cmd(children, kNodeCover, 0, pay, 12);
  b.addRepeat(20, 2, 0, 0, 80, 80, 10, children, sz, 1);
  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SceneBindingSource src{nullptr, &resolveCoverAsset, &count3};
  GfxSceneRenderer r;
  bool ok=r.render(pkg,len,src,assets,gfx);
  assert(ok);
  assert(gfx.forbiddenCalls==0);
  // Should have drawn pixels for both covers (at least some drawPixel)
  assert(gfx.drawPixelCount > 0);
  // Verify no drawBitmap file I/O was attempted
  // Also test fallback when asset missing -> draws rect placeholder
  SpyGfx gfx2; UiSceneAssets empty;
  ok=r.render(pkg,len,src,empty,gfx2); assert(ok);
  bool sawRect=false; for(auto &c: gfx2.calls) if(c.name=="drawRect"||c.name=="drawRectW") sawRect=true;
  assert(sawRect);
}

void testNamedIconWithoutAssetUsesBoundedPlaceholder(){
  SceneBuilder b;
  uint8_t pay[32]={};
  const char* name="missing_icon";
  const size_t nameLen=strlen(name);
  SceneBuilder::put16(pay+0,18);
  SceneBuilder::put16(pay+2,24);
  SceneBuilder::put16(pay+4,68);
  SceneBuilder::put16(pay+6,68);
  SceneBuilder::put16(pay+8,(uint16_t)nameLen);
  memcpy(pay+10,name,nameLen);
  pay[10+nameLen]=0; // named icon with no asset binding
  b.add(kNodeIcon,0,pay,11+nameLen);

  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SpyGfx gfx; UiSceneAssets assets;
  SceneBindingSource src{nullptr, [](const void*,BindingId,const SceneItemContext*,ResolvedValue*)->bool{return false;}, nullptr};
  assert(GfxSceneRenderer{}.render(pkg,len,src,assets,gfx));
  assert(gfx.calls.size()==1);
  assert(gfx.calls[0].name=="drawRectW");
  assert(gfx.calls[0].x==18 && gfx.calls[0].y==24);
  assert(gfx.calls[0].w==68 && gfx.calls[0].h==68);
  assert(gfx.calls[0].stroke==1);
  assert(gfx.calls[0].fill);
}

void testNoForbiddenIoInRenderPath(){
  SpyGfx gfx; UiSceneAssets assets;
  SceneBindingSource src{nullptr, &resolveInt, nullptr};
  uint8_t dummyPkg[80]={}; dummyPkg[0]=0x4D; dummyPkg[1]=0x34; dummyPkg[2]=0x54; dummyPkg[3]=0x48;
  GfxSceneRenderer r;
  // Empty package should not trigger SD/network
  bool ok=r.render(dummyPkg,sizeof(dummyPkg),src,assets,gfx);
  (void)ok; // may be false due to malformed, but forbidden must stay 0
  assert(gfx.forbiddenCalls==0);
}

void testAllNodeTypes(){
  SpyGfx gfx; UiSceneAssets assets;
  // Use the real murphy_default package which contains all node types (clear, text, battery, line, round_rect, cover, progress, repeat)
  // Provide minimal asset for cover
  static const uint8_t pix[8]={0xFF,0x81,0x81,0x81,0x81,0x81,0xFF,0x00};
  UiSceneAsset a{}; a.pixels=pix; a.width=8; a.height=8; a.stride=1;
  assets.add(AssetKey{14,kInvalidBindingId,kInvalidAssetItemIndex},a);
  SceneBindingSource src{nullptr, [](const void*,BindingId b, const SceneItemContext* item, ResolvedValue* o)->bool{
      if(b==1){ o->kind=ValueKind::Int; o->number=73; return true; }
      if(b==10){ o->kind=ValueKind::Bool; o->boolean=true; return true; }
      if(b==11){ o->kind=ValueKind::Text; o->text=TextView::fromRam("T",1); return true; }
      if(b==14){ o->kind=ValueKind::Asset; o->assetIndex=0; return true; }
      if(b==15){ o->kind=ValueKind::Int; o->number=35; return true; }
      if(item && item->valid && item->sourceBinding==20){ if(b==32||b==33){ o->kind=ValueKind::Text; o->text=TextView::fromRam("X",1); return true; } }
      if(item && item->valid && item->sourceBinding==21){ if(b==30||b==31){ o->kind=ValueKind::Text; o->text=TextView::fromRam("Y",1); return true; } }
      return false;
    }, [](const void*,BindingId s)->uint8_t{ return s==20?3: s==21?4:0; }};
  GfxSceneRenderer r; bool ok=r.render(murphy_default_m4theme, murphy_default_m4theme_len, src, assets, gfx); assert(ok);
  assert(gfx.forbiddenCalls==0);
  bool sawClear=false,sawLine=false,sawRect=false,sawRound=false,sawText=false,sawProgress=false,sawIcon=false;
  for(auto &c: gfx.calls){
    if(c.name=="clearScreen") sawClear=true;
    if(c.name=="drawLine"||c.name=="drawLineW") sawLine=true;
    if(c.name=="drawRect"||c.name=="drawRectW") sawRect=true;
    if(c.name=="drawRoundedRect") sawRound=true;
    if(c.name=="drawText") sawText=true;
    if(c.name=="fillRect"||c.name=="fillRoundedRect") sawProgress=true;
    if(c.name=="drawIcon") sawIcon=true;
  }
  assert(sawClear && sawLine && sawRect && sawRound && sawText && sawProgress);
  (void)sawIcon;
}

void testTwoLineTitleWrapsThenEllipsizes(){
  struct WidthSpy : SpyGfx {
    static int codepoints(const char* t){
      int n=0;
      for(const unsigned char* p=reinterpret_cast<const unsigned char*>(t); p && *p; ){
        ++n;
        if(*p<0x80) ++p;
        else if((*p&0xE0)==0xC0) p+=2;
        else if((*p&0xF0)==0xE0) p+=3;
        else p+=4;
      }
      return n;
    }
    int getTextWidth(int, const char* t, int=0,float=1) const { return codepoints(t)*10; }
    int getLineHeight(int) const { return 16; }
  };
  SceneBuilder b;
  // 12 ASCII chars, 10px each, rect 50x44 → 5 chars/line, 2 lines, last line ellipsis.
  b.addText("ABCDEFGHIJKL",kInvalidBindingId,0,kInvalidBindingId,kInvalidActionId,
            kInvalidBindingId,10,20,16,50,0,44);
  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SceneBindingSource src{nullptr, [](const void*,BindingId, const SceneItemContext*, ResolvedValue*)->bool{return false;}, nullptr};
  WidthSpy gfx; UiSceneAssets assets;
  assert(GfxSceneRenderer{}.render(pkg,len,src,assets,gfx));
  int textCalls=0;
  for(auto &c: gfx.calls) if(c.name=="drawText") ++textCalls;
  assert(textCalls==2);
  assert(gfx.calls[0].name=="drawText" && gfx.calls[0].y==20);
  assert(gfx.calls[0].text=="ABCDE");
  assert(gfx.calls[1].name=="drawText" && gfx.calls[1].y==36);
  assert(gfx.calls[1].text==std::string("FGHI")+"\xE2\x80\xA6");
}

void testShortTitleStaysOneLineInTallRect(){
  struct WidthSpy : SpyGfx {
    int getTextWidth(int, const char* t, int=0,float=1) const {
      int n=0; if(t) while(t[n]) ++n; return n*10;
    }
    int getLineHeight(int) const { return 16; }
  };
  SceneBuilder b;
  b.addText("Hi",kInvalidBindingId,0,kInvalidBindingId,kInvalidActionId,
            kInvalidBindingId,10,20,16,50,0,44);
  uint8_t pkg[2304]={}; size_t len=b.finish(pkg);
  SceneBindingSource src{nullptr, [](const void*,BindingId, const SceneItemContext*, ResolvedValue*)->bool{return false;}, nullptr};
  WidthSpy gfx; UiSceneAssets assets;
  assert(GfxSceneRenderer{}.render(pkg,len,src,assets,gfx));
  assert(gfx.calls.size()==1);
  assert(gfx.calls[0].name=="drawText" && gfx.calls[0].text=="Hi");
}

void testPackageBitmapOverlay(){
  SpyGfx gfx; UiSceneAssets assets;
  static const uint8_t pix[8]={0xFF,0x81,0x81,0x81,0x81,0x81,0xFF,0x00};
  UiSceneAsset a{}; a.pixels=pix; a.width=8; a.height=8; a.stride=1;
  GfxSceneRenderer::draw1BitAsset(gfx,a,0,0);
  // FF=8, 81=2 each, 6*2=12, plus FF=8 plus last 0 => 28? Actually 8+5*2+8=26 for this pattern (last row 0)
  assert(gfx.drawPixelCount== 26);
}

int main(){
  testAssetsRequireExactBindingAndItemContext();
  testSceneFontIdRoutesToRuntimeFontId();
  testTextAlignmentUsesMeasuredWidthWithinRect();
  testOrderedBitmapSemantics();
  testBlackInkNoOpWhite();
  testCoverUsesCachedAssetOnly();
  testNamedIconWithoutAssetUsesBoundedPlaceholder();
  testNoForbiddenIoInRenderPath();
  testAllNodeTypes();
  testTwoLineTitleWrapsThenEllipsizes();
  testShortTitleStaysOneLineInTallRect();
  testPackageBitmapOverlay();
  // Final integration: render real murphy_default with spy and check all node types emit without forbidden I/O
  {
    SpyGfx gfx; UiSceneAssets assets;
    SceneBindingSource src{nullptr, [](const void*,BindingId b, const SceneItemContext* item, ResolvedValue* o)->bool{
      if(b==1){ o->kind=ValueKind::Int; o->number=73; return true; }
      if(b==10){ o->kind=ValueKind::Bool; o->boolean=true; return true; }
      if(b==11){ o->kind=ValueKind::Text; o->text=TextView::fromRam("T",1); return true; }
      if(b==14){ o->kind=ValueKind::Asset; o->assetIndex=0; return true; }
      if(b==15){ o->kind=ValueKind::Int; o->number=35; return true; }
      if(item && item->valid && item->sourceBinding==20){ if(b==32||b==33){ o->kind=ValueKind::Text; o->text=TextView::fromRam("X",1); return true; } }
      if(item && item->valid && item->sourceBinding==21){ if(b==30||b==31){ o->kind=ValueKind::Text; o->text=TextView::fromRam("Y",1); return true; } }
      return false;
    }, [](const void*,BindingId s)->uint8_t{ return s==20?3: s==21?4:0; }};
    static const uint8_t pix[8]={0xFF,0x81,0x81,0x81,0x81,0x81,0xFF,0x00};
    UiSceneAsset a{}; a.pixels=pix; a.width=8; a.height=8; a.stride=1;
    assets.add(AssetKey{14,kInvalidBindingId,kInvalidAssetItemIndex},a);
    GfxSceneRenderer r; bool ok=r.render(murphy_default_m4theme, murphy_default_m4theme_len, src, assets, gfx);
    assert(ok);
    assert(gfx.forbiddenCalls==0);
  }
  return 0;
}

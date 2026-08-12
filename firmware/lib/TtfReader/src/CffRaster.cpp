#include "CffReader.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif
namespace ttf { namespace {
void* reallocPsramFirst(void* p,size_t n){if(!n)return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  void* q=heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!q) q=heap_caps_realloc(p,n,MALLOC_CAP_8BIT);
  return q;
#else
  return std::realloc(p,n);
#endif
}
void freeMem(void* p){if(!p)return;
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}
bool differs(const Pt&a,const Pt&b){return std::fabs(a.x-b.x)>0.001f||std::fabs(a.y-b.y)>0.001f;}
}
CffFont::~CffFont(){clearScratch();freeMem(cmapData_);cmapData_=nullptr;cmapLen_=cmapScratchCap_=0;}
void CffFont::clearScratch(){
  freeMem(type2Scratch_); type2Scratch_=nullptr; type2ScratchCap_=0;
  freeMem(covScratch_); covScratch_=nullptr; covScratchCap_=0;
  freeMem(packedScratch_); packedScratch_=nullptr; packedScratchCap_=0;
  freeMem(intersectionScratch_); intersectionScratch_=nullptr; intersectionScratchCap_=0;
}
bool CffFont::glyphPixelBox(uint16_t gid,uint16_t sizePx,int&x0,int&y0,int&x1,int&y1) const{
  x0=y0=x1=y1=0; std::vector<Contour> c; if(!collectGlyph(gid,c))return false;
  const float s=float(sizePx)/float(unitsPerEm_); bool first=true; float mnx=0,mxx=0,mny=0,mxy=0;
  for(const auto&ct:c)for(const auto&p:ct.pts){if(first){mnx=mxx=p.x;mny=mxy=p.y;first=false;}else{mnx=std::min(mnx,p.x);mxx=std::max(mxx,p.x);mny=std::min(mny,p.y);mxy=std::max(mxy,p.y);}}
  if(first)return true; x0=int(std::floor(mnx*s)); y0=int(std::floor(-mxy*s)); x1=int(std::ceil(mxx*s)); y1=int(std::ceil(-mny*s)); return true;
}
bool CffFont::rasterize(uint16_t gid,uint16_t sizePx,GlyphBitmap&out){
  out={}; if(!ready_||gid>=glyphCount_||!unitsPerEm_)return false;
  int32_t adv=0,lsb=0; if(!glyphHMetrics(gid,adv,lsb))return false;
  const float scale=float(sizePx)/float(unitsPerEm_); out.advance=int16_t(std::lround(adv*scale));
  std::vector<Contour> contours; if(!collectGlyph(gid,contours))return false;
  bool first=true; float mnx=0,mxx=0,mny=0,mxy=0; uint32_t segmentCount=0;
  for(const auto&c:contours){
    for(const auto&p:c.pts){if(first){mnx=mxx=p.x;mny=mxy=p.y;first=false;}else{mnx=std::min(mnx,p.x);mxx=std::max(mxx,p.x);mny=std::min(mny,p.y);mxy=std::max(mxy,p.y);}}
    if(c.pts.size()>1){segmentCount+=uint32_t(c.pts.size()-1);if(differs(c.pts.back(),c.pts.front()))++segmentCount;}
  }
  if(first)return true;
  const int x0=int(std::floor(mnx*scale)), y0=int(std::floor(-mxy*scale));
  const int x1=int(std::ceil(mxx*scale)), y1=int(std::ceil(-mny*scale));
  const int w=x1-x0,h=y1-y0; if(w<=0||h<=0)return true;
  if(w>255||h>255){lastError_="CFF glyph larger than 255px";return false;}
  const uint32_t pixels=uint32_t(w)*uint32_t(h);
  if(pixels>covScratchCap_){auto* p=(uint8_t*)reallocPsramFirst(covScratch_,pixels);if(!p){lastError_="CFF coverage scratch OOM";return false;}covScratch_=p;covScratchCap_=pixels;}
  std::memset(covScratch_,0,pixels);
  if(segmentCount>intersectionScratchCap_){
    if(segmentCount>65535u){lastError_="CFF glyph edge count exceeds safety limit";return false;}
    auto* p=(Intersection*)reallocPsramFirst(intersectionScratch_,size_t(segmentCount)*sizeof(Intersection));
    if(!p){lastError_="CFF intersection scratch OOM";return false;}
    intersectionScratch_=p;intersectionScratchCap_=segmentCount;
  }
  for(int py=0;py<h;++py)for(int sub=0;sub<2;++sub){
    const float yy=py+(sub?0.75f:0.25f); uint32_t count=0;
    auto addEdge=[&](const Pt&a,const Pt&b){
      const float ax=a.x*scale-x0, ay=-a.y*scale-y0, bx=b.x*scale-x0, by=-b.y*scale-y0;
      const float ymin=std::min(ay,by),ymax=std::max(ay,by);
      if(yy<ymin||yy>=ymax||std::fabs(by-ay)<=1e-6f)return;
      if(count>=intersectionScratchCap_)return;
      intersectionScratch_[count].x=ax+(yy-ay)*(bx-ax)/(by-ay);
      intersectionScratch_[count].sign=(by>ay)?1:-1; ++count;
    };
    for(const auto&c:contours){if(c.pts.size()<2)continue;for(size_t i=0;i+1<c.pts.size();++i)addEdge(c.pts[i],c.pts[i+1]);if(differs(c.pts.back(),c.pts.front()))addEdge(c.pts.back(),c.pts.front());}
    for(uint32_t i=1;i<count;++i){Intersection v=intersectionScratch_[i];uint32_t j=i;while(j&&intersectionScratch_[j-1].x>v.x){intersectionScratch_[j]=intersectionScratch_[j-1];--j;}intersectionScratch_[j]=v;}
    int winding=0;
    for(uint32_t i=0;i+1<count;++i){winding+=intersectionScratch_[i].sign;if(!winding)continue;float a=intersectionScratch_[i].x,b=intersectionScratch_[i+1].x;if(b<a)std::swap(a,b);for(int px=int(std::floor(a));px<int(std::ceil(b));++px){if(px<0||px>=w)continue;const float lo=std::max(a,float(px)),hi=std::min(b,float(px+1));if(hi>lo){uint8_t&cv=covScratch_[size_t(py)*w+px];const int add=int((hi-lo)*4.f+0.5f);cv=uint8_t(std::min(8,int(cv)+add));}}}
  }
  const uint32_t packedLen=(pixels+3)/4;
  if(packedLen>packedScratchCap_){auto*p=(uint8_t*)reallocPsramFirst(packedScratch_,packedLen);if(!p){lastError_="CFF packed scratch OOM";return false;}packedScratch_=p;packedScratchCap_=packedLen;}
  std::memset(packedScratch_,0,packedLen);
  for(uint32_t i=0;i<pixels;++i){uint8_t level=uint8_t((uint32_t(covScratch_[i])*3u+4u)/8u);if(level>3)level=3;packedScratch_[i/4]|=uint8_t(level<<((3-(i&3u))*2));}
  out.data=packedScratch_;out.width=w;out.height=h;out.xoff=x0;out.yoff=-y0;out.packedLen=uint16_t(packedLen);lastError_="ok";return true;
}
} // namespace ttf

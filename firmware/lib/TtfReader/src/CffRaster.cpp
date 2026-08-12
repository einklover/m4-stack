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
void* allocInternal(size_t n){if(!n)return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
return heap_caps_malloc(n,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
return std::malloc(n);
#endif
}
void* reallocInternal(void*p,size_t n){
#if defined(ARDUINO_ARCH_ESP32)
return heap_caps_realloc(p,n,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
return std::realloc(p,n);
#endif
}
void* reallocPsram(void*p,size_t n){
#if defined(ARDUINO_ARCH_ESP32)
void*q=heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); if(!q)q=heap_caps_realloc(p,n,MALLOC_CAP_8BIT); return q;
#else
return std::realloc(p,n);
#endif
}
void freeMem(void*p){if(!p)return;
#if defined(ARDUINO_ARCH_ESP32)
heap_caps_free(p);
#else
std::free(p);
#endif
}
struct Seg{float x0,y0,x1,y1;};
}
CffFont::~CffFont(){clearScratch();}
void CffFont::clearScratch(){freeMem(covScratch_);covScratch_=nullptr;covScratchCap_=0;freeMem(packedScratch_);packedScratch_=nullptr;packedScratchCap_=0;}
bool CffFont::glyphPixelBox(uint16_t gid,uint16_t sizePx,int&x0,int&y0,int&x1,int&y1) const{x0=y0=x1=y1=0;std::vector<Contour> c;if(!collectGlyph(gid,c))return false;float s=float(sizePx)/float(unitsPerEm_);bool first=true;float mnx=0,mxx=0,mny=0,mxy=0;for(auto&ct:c)for(auto&p:ct.pts){if(first){mnx=mxx=p.x;mny=mxy=p.y;first=false;}else{mnx=std::min(mnx,p.x);mxx=std::max(mxx,p.x);mny=std::min(mny,p.y);mxy=std::max(mxy,p.y);}}if(first)return true;x0=int(std::floor(mnx*s));y0=int(std::floor(-mxy*s));x1=int(std::ceil(mxx*s));y1=int(std::ceil(-mny*s));return true;}
bool CffFont::rasterize(uint16_t gid,uint16_t sizePx,GlyphBitmap&out){out={};if(!ready_||gid>=glyphCount_||!unitsPerEm_)return false;int32_t adv=0,lsb=0;if(!glyphHMetrics(gid,adv,lsb))return false;float scale=float(sizePx)/float(unitsPerEm_);out.advance=int16_t(std::lround(adv*scale));std::vector<Contour> contours;if(!collectGlyph(gid,contours))return false;bool first=true;float mnx=0,mxx=0,mny=0,mxy=0;for(auto&c:contours)for(auto&p:c.pts){if(first){mnx=mxx=p.x;mny=mxy=p.y;first=false;}else{mnx=std::min(mnx,p.x);mxx=std::max(mxx,p.x);mny=std::min(mny,p.y);mxy=std::max(mxy,p.y);}}if(first)return true;int x0=int(std::floor(mnx*scale)),y0=int(std::floor(-mxy*scale)),x1=int(std::ceil(mxx*scale)),y1=int(std::ceil(-mny*scale));int w=x1-x0,h=y1-y0;if(w<=0||h<=0)return true;if(w>255||h>255){lastError_="CFF glyph larger than 255px";return false;}std::vector<Seg> segs;for(auto&c:contours){if(c.pts.size()<2)continue;for(size_t i=0;i+1<c.pts.size();++i){auto&a=c.pts[i];auto&b=c.pts[i+1];segs.push_back({a.x*scale-x0,-a.y*scale-y0,b.x*scale-x0,-b.y*scale-y0});}auto&a=c.pts.back();auto&b=c.pts.front();if(std::fabs(a.x-b.x)>0.001f||std::fabs(a.y-b.y)>0.001f)segs.push_back({a.x*scale-x0,-a.y*scale-y0,b.x*scale-x0,-b.y*scale-y0});}uint32_t np=uint32_t(w)*uint32_t(h);if(np>covScratchCap_){auto*nb=(uint8_t*)reallocInternal(covScratch_,np);if(!nb)return false;covScratch_=nb;covScratchCap_=np;}std::memset(covScratch_,0,np);std::vector<float> xs;std::vector<int8_t> signs;xs.reserve(segs.size());signs.reserve(segs.size());for(int py=0;py<h;++py)for(int sub=0;sub<2;++sub){float yy=py+(sub?0.75f:0.25f);xs.clear();signs.clear();for(auto&s:segs){float ymin=std::min(s.y0,s.y1),ymax=std::max(s.y0,s.y1);if(yy>=ymin&&yy<ymax&&std::fabs(s.y1-s.y0)>1e-6f){xs.push_back(s.x0+(yy-s.y0)*(s.x1-s.x0)/(s.y1-s.y0));signs.push_back(s.y1>s.y0?1:-1);}}for(size_t i=1;i<xs.size();++i){float v=xs[i];int8_t sg=signs[i];size_t j=i;while(j&&xs[j-1]>v){xs[j]=xs[j-1];signs[j]=signs[j-1];--j;}xs[j]=v;signs[j]=sg;}int winding=0;for(size_t i=0;i+1<xs.size();++i){winding+=signs[i];if(!winding)continue;float a=xs[i],b=xs[i+1];for(int px=int(std::floor(a));px<int(std::ceil(b));++px){if(px<0||px>=w)continue;float lo=std::max(a,float(px)),hi=std::min(b,float(px+1));if(hi>lo)covScratch_[size_t(py)*w+px]+=uint8_t((hi-lo)*4.f+0.5f);}}}uint32_t plen=(np+3)/4;if(plen>packedScratchCap_){auto*nb=(uint8_t*)reallocPsram(packedScratch_,plen);if(!nb)return false;packedScratch_=nb;packedScratchCap_=plen;}std::memset(packedScratch_,0,plen);for(uint32_t i=0;i<np;++i){uint8_t lvl=uint8_t((uint32_t(covScratch_[i])*3u+7u)/8u);if(lvl>3)lvl=3;packedScratch_[i/4]|=uint8_t(lvl<<((3-(i%4))*2));}out.data=packedScratch_;out.width=w;out.height=h;out.xoff=x0;out.yoff=-y0;out.packedLen=uint16_t(plen);lastError_="ok";return true;}
} // namespace ttf

#include "CffReader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace ttf {
namespace {
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]); }
uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void* reallocPsramFirst(void* p, size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!q) q = heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  return q;
#else
  return std::realloc(p, n);
#endif
}
bool type2Number(const uint8_t* data, size_t len, size_t& pos, float& out) {
  if (pos >= len) return false;
  const uint8_t b0 = data[pos++];
  if (b0 >= 32 && b0 <= 246) { out = float(int(b0) - 139); return true; }
  if (b0 >= 247 && b0 <= 250) { if (pos >= len) return false; out = float((b0 - 247) * 256 + data[pos++] + 108); return true; }
  if (b0 >= 251 && b0 <= 254) { if (pos >= len) return false; out = float(-(int(b0) - 251) * 256 - data[pos++] - 108); return true; }
  if (b0 == 28) { if (len - pos < 2) return false; out = float(static_cast<int16_t>(rd16(data + pos))); pos += 2; return true; }
  if (b0 == 255) { if (len - pos < 4) return false; out = float(static_cast<int32_t>(rd32(data + pos))) / 65536.0f; pos += 4; return true; }
  return false;
}
int subrBias(uint16_t count) { return count < 1240 ? 107 : (count < 33900 ? 1131 : 32768); }
void debugMove(std::vector<Contour>* out, float x, float y) {
  if (!out) return;
  Contour c; c.pts.push_back({x,y,true}); out->push_back(std::move(c));
}
void debugLine(std::vector<Contour>* out, float x, float y) {
  if (!out) return;
  if (out->empty() || out->back().pts.empty()) debugMove(out,x,y);
  else { auto& p=out->back().pts; if(std::fabs(p.back().x-x)>1e-6f||std::fabs(p.back().y-y)>1e-6f)p.push_back({x,y,true}); }
}
}  // namespace

bool CffFont::appendEdge(EdgeBuildState& s,float x0,float y0,float x1,float y1) const {
  if(std::fabs(x1-x0)<=1e-6f&&std::fabs(y1-y0)<=1e-6f)return true;
  if(s.count>=65535u){lastError_="CFF edge safety limit exceeded";return false;}
  if(s.count>=edgeScratchCap_){uint32_t cap=edgeScratchCap_?edgeScratchCap_*2u:128u;if(cap<=s.count)cap=s.count+1;if(cap>65535u)cap=65535u;auto*p=static_cast<Edge*>(reallocPsramFirst(edgeScratch_,size_t(cap)*sizeof(Edge)));if(!p){lastError_="CFF edge PSRAM scratch OOM";return false;}edgeScratch_=p;edgeScratchCap_=cap;}
  edgeScratch_[s.count++]={x0,y0,x1,y1};
  return true;
}

bool CffFont::edgeClose(EdgeBuildState& s) const {
  if(!s.open)return true;
  if(!appendEdge(s,s.lastX,s.lastY,s.startX,s.startY))return false;
  s.open=false;
  return true;
}

bool CffFont::edgeMove(EdgeBuildState& s,float x,float y) const {
  if(s.open&&!edgeClose(s))return false;
  s.open=true;s.startX=s.lastX=x;s.startY=s.lastY=y;
  if(!s.haveBounds){s.minX=s.maxX=x;s.minY=s.maxY=y;s.haveBounds=true;}
  else{s.minX=std::min(s.minX,x);s.maxX=std::max(s.maxX,x);s.minY=std::min(s.minY,y);s.maxY=std::max(s.maxY,y);}
  return true;
}

bool CffFont::edgeLine(EdgeBuildState& s,float x,float y) const {
  if(!s.open){if(!edgeMove(s,x,y))return false;return true;}
  if(!appendEdge(s,s.lastX,s.lastY,x,y))return false;
  s.lastX=x;s.lastY=y;
  s.minX=std::min(s.minX,x);s.maxX=std::max(s.maxX,x);s.minY=std::min(s.minY,y);s.maxY=std::max(s.maxY,y);
  return true;
}

bool CffFont::parsePrivateDict() {
  localSubrsInfo_={}; localSubrs_={};
  if(!privateDict_.valid()) return true;
  if(privateDict_.len>64u*1024u){lastError_="CFF Private DICT too large";return false;}
  // Legacy compatibility entry point. Normal init uses parsePrivateSubrs(),
  // which keeps DICT bytes in the reusable PSRAM Type2 scratch.
  if(privateDict_.len>type2ScratchCap_){auto*p=static_cast<uint8_t*>(reallocPsramFirst(type2Scratch_,privateDict_.len));if(!p)return false;type2Scratch_=p;type2ScratchCap_=privateDict_.len;}
  if(!readAt(privateDict_.off,type2Scratch_,privateDict_.len)){lastError_="failed to read CFF Private DICT";return false;}
  const uint8_t* bytes=type2Scratch_;int32_t stack[48];size_t sp=0,pos=0;int32_t subrsRel=-1;
  while(pos<privateDict_.len){const uint8_t b0=bytes[pos];if(b0>=32||b0==28||b0==29){float fv=0;if(!type2Number(bytes,privateDict_.len,pos,fv)||sp>=48)return false;stack[sp++]=int32_t(std::lround(fv));continue;}++pos;uint16_t op=b0;if(b0==12){if(pos>=privateDict_.len)return false;op=uint16_t(0x0c00|bytes[pos++]);}if(op==19){if(sp!=1||stack[0]<0)return false;subrsRel=stack[0];}sp=0;}
  if(subrsRel>=0){const uint64_t abs=uint64_t(privateDict_.off)+uint32_t(subrsRel);if(abs<cff_.off||abs>=uint64_t(cff_.off)+cff_.len)return false;if(!parseIndex(uint32_t(abs-cff_.off),localSubrsInfo_,nullptr))return false;localSubrs_=localSubrsInfo_.whole;}
  return true;
}

bool CffFont::executeType2(Slice code,std::vector<Contour>* debugOut,EdgeBuildState* edgeOut,int depth,float& x,float& y,uint32_t& stemCount) const {
  static constexpr uint32_t kMaxCode=256u*1024u,kMaxActive=512u*1024u; static constexpr int kMaxFrames=17;
  if(depth>16||!code.valid()||code.len>kMaxCode){lastError_="CFF Type2 recursion or CharString limit exceeded";return false;}
  struct Frame{uint32_t base=0,len=0,pos=0;};
  Frame frames[kMaxFrames];int frameCount=0;uint32_t used=0;
  float stack[48];size_t sp=0;
  // Type2 transient array has exactly 32 entries. Keeping it automatic avoids
  // any heap allocation even for arithmetic-heavy CFF subroutines.
  float transient[32]={};
  uint32_t randomState=0x6d2b79f5u^code.off^code.len;
  auto pushFrame=[&](Slice slice)->bool{if(frameCount>=kMaxFrames||!slice.valid()||slice.len>kMaxCode||used>kMaxActive||slice.len>kMaxActive-used){lastError_="CFF Type2 active bytecode limit exceeded";return false;}uint32_t need=used+slice.len;if(need>type2ScratchCap_){auto*p=static_cast<uint8_t*>(reallocPsramFirst(type2Scratch_,need));if(!p){lastError_="CFF Type2 PSRAM scratch OOM";return false;}type2Scratch_=p;type2ScratchCap_=need;}if(!readAt(slice.off,type2Scratch_+used,slice.len)){lastError_="failed to read CFF Type2 CharString";return false;}frames[frameCount++]={used,slice.len,0};used=need;return true;};
  auto emitMove=[&](float nx,float ny)->bool{debugMove(debugOut,nx,ny);return !edgeOut||edgeMove(*edgeOut,nx,ny);};
  auto emitLine=[&](float nx,float ny)->bool{debugLine(debugOut,nx,ny);return !edgeOut||edgeLine(*edgeOut,nx,ny);};
  auto emitCubic=[&](float dx1,float dy1,float dx2,float dy2,float dx3,float dy3)->bool{const float x0=x,y0=y,x1=x0+dx1,y1=y0+dy1,x2=x1+dx2,y2=y1+dy2,x3=x2+dx3,y3=y2+dy3;const float cx=x3-x0,cy=y3-y0,chord=std::sqrt(cx*cx+cy*cy);const float d1=std::fabs((x1-x0)*cy-(y1-y0)*cx)/std::max(chord,1.0f),d2=std::fabs((x2-x0)*cy-(y2-y0)*cx)/std::max(chord,1.0f);const int steps=std::max(2,std::min(24,2+int(std::max(d1,d2)/20.0f)));for(int i=1;i<=steps;++i){float t=float(i)/steps,mt=1.0f-t,px=mt*mt*mt*x0+3*mt*mt*t*x1+3*mt*t*t*x2+t*t*t*x3,py=mt*mt*mt*y0+3*mt*mt*t*y1+3*mt*t*t*y2+t*t*t*y3;if(!emitLine(px,py))return false;}x=x3;y=y3;return true;};
  auto push=[&](float v)->bool{if(sp>=48)return false;stack[sp++]=v;return true;};
  auto binary=[&](char which)->bool{if(sp<2)return false;const float a=stack[sp-2],b=stack[sp-1];sp-=2;float r=0;switch(which){case '+':r=a+b;break;case '-':r=a-b;break;case '*':r=a*b;break;case '/':if(std::fabs(b)<1e-12f)return false;r=a/b;break;default:return false;}return push(r);};
  if(!pushFrame(code))return false;
  while(frameCount>0){
    Frame&f=frames[frameCount-1];if(f.pos>=f.len){used=f.base;--frameCount;continue;}uint8_t*bytes=type2Scratch_+f.base;size_t pos=f.pos;uint8_t b0=bytes[pos];
    if(b0==28||b0==255||b0>=32){float v=0;if(!type2Number(bytes,f.len,pos,v)||!push(v)){lastError_="malformed CFF Type2 operand";return false;}f.pos=uint32_t(pos);continue;}
    ++pos;uint16_t op=b0;if(b0==12){if(pos>=f.len)return false;op=uint16_t(0x0c00|bytes[pos++]);}f.pos=uint32_t(pos);auto clear=[&](){sp=0;};auto stem=[&](){stemCount+=uint32_t((sp-(sp&1u))/2u);clear();};
    switch(op){
      case 1:case 3:case 18:case 23:stem();break;
      case 19:case 20:{if(sp)stem();size_t m=(stemCount+7u)/8u;if(f.len-f.pos<m)return false;f.pos+=uint32_t(m);break;}
      case 4:if(!sp)return false;y+=stack[sp-1];if(!emitMove(x,y))return false;clear();break;
      case 21:if(sp<2)return false;x+=stack[sp-2];y+=stack[sp-1];if(!emitMove(x,y))return false;clear();break;
      case 22:if(!sp)return false;x+=stack[sp-1];if(!emitMove(x,y))return false;clear();break;
      case 5:if(sp<2||(sp&1u))return false;for(size_t i=0;i<sp;i+=2){x+=stack[i];y+=stack[i+1];if(!emitLine(x,y))return false;}clear();break;
      case 6:case 7:{bool h=op==6;for(size_t i=0;i<sp;++i){if(h)x+=stack[i];else y+=stack[i];if(!emitLine(x,y))return false;h=!h;}clear();break;}
      case 8:if(sp<6||sp%6)return false;for(size_t i=0;i<sp;i+=6)if(!emitCubic(stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]))return false;clear();break;
      case 24:{if(sp<8||(sp-2)%6)return false;size_t i=0;for(;i+2<sp;i+=6)if(!emitCubic(stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]))return false;x+=stack[i];y+=stack[i+1];if(!emitLine(x,y))return false;clear();break;}
      case 25:{if(sp<8||(sp-6)%2)return false;size_t i=0;for(;i+6<sp;i+=2){x+=stack[i];y+=stack[i+1];if(!emitLine(x,y))return false;}if(!emitCubic(stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]))return false;clear();break;}
      case 26:{size_t i=0;float dx=0;if(sp&1u)dx=stack[i++];if(sp-i<4||(sp-i)%4)return false;for(;i<sp;i+=4){if(!emitCubic(dx,stack[i],stack[i+1],stack[i+2],0,stack[i+3]))return false;dx=0;}clear();break;}
      case 27:{size_t i=0;float dy=0;if(sp&1u)dy=stack[i++];if(sp-i<4||(sp-i)%4)return false;for(;i<sp;i+=4){if(!emitCubic(stack[i],dy,stack[i+1],stack[i+2],stack[i+3],0))return false;dy=0;}clear();break;}
      case 30:case 31:{if(sp<4)return false;bool hf=op==31;size_t i=0;while(sp-i>=4){bool last=sp-i==5||sp-i==4;float a=stack[i],b=stack[i+1],c=stack[i+2],d=stack[i+3],e=last&&sp-i==5?stack[i+4]:0;if(hf){if(!emitCubic(a,0,b,c,e,d))return false;}else if(!emitCubic(0,a,b,c,d,e))return false;i+=(last&&sp-i==5)?5:4;hf=!hf;}if(i!=sp)return false;clear();break;}
      case 10:case 29:{if(!sp)return false;int raw=int(std::lround(stack[--sp]));const IndexInfo&idx=op==10?localSubrsInfo_:globalSubrsInfo_;if(!idx.count)return false;int bi=raw+subrBias(idx.count);if(bi<0||bi>=idx.count)return false;Slice sub;if(!indexObject(idx,uint16_t(bi),sub)||!pushFrame(sub))return false;break;}
      case 11:used=f.base;--frameCount;break;
      case 14:clear();if(edgeOut&&!edgeClose(*edgeOut))return false;return true;

      // Escaped Type2 stack/arithmetic operators. All state is fixed-size.
      case 0x0c00:break; // dotsection (obsolete no-op)
      case 0x0c03:if(sp<2)return false;{float b=stack[--sp],a=stack[--sp];if(!push((a!=0&&b!=0)?1.f:0.f))return false;}break;
      case 0x0c04:if(sp<2)return false;{float b=stack[--sp],a=stack[--sp];if(!push((a!=0||b!=0)?1.f:0.f))return false;}break;
      case 0x0c05:if(!sp)return false;stack[sp-1]=stack[sp-1]==0?1.f:0.f;break;
      case 0x0c09:if(!sp)return false;stack[sp-1]=std::fabs(stack[sp-1]);break;
      case 0x0c0a:if(!binary('+'))return false;break;
      case 0x0c0b:if(!binary('-'))return false;break;
      case 0x0c0c:if(!binary('/'))return false;break;
      case 0x0c0e:if(!sp)return false;stack[sp-1]=-stack[sp-1];break;
      case 0x0c0f:if(sp<2)return false;{float b=stack[--sp],a=stack[--sp];if(!push(std::fabs(a-b)<1e-6f?1.f:0.f))return false;}break;
      case 0x0c12:if(!sp)return false;--sp;break; // drop
      case 0x0c14:{if(sp<2)return false;int i=int(std::lround(stack[sp-1]));float v=stack[sp-2];sp-=2;if(i<0||i>=32)return false;transient[i]=v;break;}
      case 0x0c15:{if(!sp)return false;int i=int(std::lround(stack[sp-1]));if(i<0||i>=32)return false;stack[sp-1]=transient[i];break;}
      case 0x0c16:{if(sp<4)return false;float v2=stack[--sp],v1=stack[--sp],s2=stack[--sp],s1=stack[--sp];if(!push(v1<=v2?s1:s2))return false;break;}
      case 0x0c17:{randomState=randomState*1664525u+1013904223u;if(!push(float((randomState>>8)&0x00ffffffu)/16777216.0f))return false;break;}
      case 0x0c18:if(!binary('*'))return false;break;
      case 0x0c1a:if(!sp||stack[sp-1]<0)return false;stack[sp-1]=std::sqrt(stack[sp-1]);break;
      case 0x0c1b:if(!sp||sp>=48)return false;stack[sp]=stack[sp-1];++sp;break;
      case 0x0c1c:if(sp<2)return false;std::swap(stack[sp-1],stack[sp-2]);break;
      case 0x0c1d:{if(!sp)return false;int i=int(std::lround(stack[--sp]));if(!sp)return false;if(i<0)i=0;if(i>=int(sp))i=int(sp)-1;if(!push(stack[sp-1-size_t(i)]))return false;break;}
      case 0x0c1e:{if(sp<2)return false;int j=int(std::lround(stack[--sp]));int n=int(std::lround(stack[--sp]));if(n<0||n>int(sp))return false;if(n<=1)break;j%=n;if(j<0)j+=n;for(int k=0;k<j;++k){float last=stack[sp-1];for(size_t q=sp-1;q>sp-size_t(n);--q)stack[q]=stack[q-1];stack[sp-size_t(n)]=last;}break;}

      case 0x0c22:if(sp!=7)return false;if(!emitCubic(stack[0],0,stack[1],stack[2],stack[3],0)||!emitCubic(stack[4],0,stack[5],-stack[2],stack[6],0))return false;clear();break;
      case 0x0c23:if(sp!=13)return false;if(!emitCubic(stack[0],stack[1],stack[2],stack[3],stack[4],stack[5])||!emitCubic(stack[6],stack[7],stack[8],stack[9],stack[10],stack[11]))return false;clear();break;
      case 0x0c24:if(sp!=9)return false;if(!emitCubic(stack[0],stack[1],stack[2],stack[3],stack[4],0)||!emitCubic(stack[5],0,stack[6],stack[7],stack[8],-(stack[1]+stack[3]+stack[7])))return false;clear();break;
      case 0x0c25:{if(sp!=11)return false;float dx=stack[0]+stack[2]+stack[4]+stack[6]+stack[8],dy=stack[1]+stack[3]+stack[5]+stack[7]+stack[9];bool xd=std::fabs(dx)>std::fabs(dy);if(!emitCubic(stack[0],stack[1],stack[2],stack[3],stack[4],stack[5])||!emitCubic(stack[6],stack[7],stack[8],stack[9],xd?stack[10]:-dx,xd?-dy:stack[10]))return false;clear();break;}
      default:lastError_="unsupported CFF Type2 operator";return false;
    }
  }
  if(edgeOut&&!edgeClose(*edgeOut))return false;
  return true;
}

bool CffFont::collectGlyph(uint16_t gid,std::vector<Contour>& out) const {
  if(!ready_||gid>=glyphCount_)return false;
  // CID runtime callers normally use rasterize()/glyphPixelBox(), which select
  // the FD before entering the VM. Keep debug collectGlyph useful as well.
  if(!prepareGlyphLocalSubrs(gid))return false;
  Slice glyph;if(!indexObject(charStringsInfo_,gid,glyph))return false;float x=0,y=0;uint32_t stems=0;if(!executeType2(glyph,&out,nullptr,0,x,y,stems))return false;lastError_="ok";return true;
}

bool CffFont::collectEdges(uint16_t gid,EdgeBuildState& state) const {
  state={};if(!ready_||gid>=glyphCount_)return false;Slice glyph;if(!indexObject(charStringsInfo_,gid,glyph))return false;float x=0,y=0;uint32_t stems=0;if(!executeType2(glyph,nullptr,&state,0,x,y,stems))return false;if(!edgeClose(state))return false;lastError_="ok";return true;
}
} // namespace ttf

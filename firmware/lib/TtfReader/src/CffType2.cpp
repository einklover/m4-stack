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
uint32_t rd32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
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
  if (b0 >= 247 && b0 <= 250) {
    if (pos >= len) return false;
    out = float((b0 - 247) * 256 + data[pos++] + 108); return true;
  }
  if (b0 >= 251 && b0 <= 254) {
    if (pos >= len) return false;
    out = float(-(int(b0) - 251) * 256 - data[pos++] - 108); return true;
  }
  if (b0 == 28) {
    if (len - pos < 2) return false;
    out = float(static_cast<int16_t>(rd16(data + pos))); pos += 2; return true;
  }
  if (b0 == 255) {
    if (len - pos < 4) return false;
    out = float(static_cast<int32_t>(rd32(data + pos))) / 65536.0f; pos += 4; return true;
  }
  return false;
}
int subrBias(uint16_t count) { return count < 1240 ? 107 : (count < 33900 ? 1131 : 32768); }
void ensureContour(std::vector<Contour>& out, float x, float y) {
  if (out.empty() || out.back().pts.empty()) { Contour c; c.pts.push_back({x,y,true}); out.push_back(std::move(c)); }
}
void moveTo(std::vector<Contour>& out, float x, float y) { Contour c; c.pts.push_back({x,y,true}); out.push_back(std::move(c)); }
void lineTo(std::vector<Contour>& out, float x, float y) {
  ensureContour(out,x,y); auto& pts=out.back().pts;
  if (pts.empty() || std::fabs(pts.back().x-x)>1e-6f || std::fabs(pts.back().y-y)>1e-6f) pts.push_back({x,y,true});
}
void cubicTo(std::vector<Contour>& out, float& x, float& y,
             float dx1,float dy1,float dx2,float dy2,float dx3,float dy3) {
  ensureContour(out,x,y);
  const float x0=x,y0=y,x1=x0+dx1,y1=y0+dy1,x2=x1+dx2,y2=y1+dy2,x3=x2+dx3,y3=y2+dy3;
  const float chordX=x3-x0,chordY=y3-y0,chord=std::sqrt(chordX*chordX+chordY*chordY);
  const float d1=std::fabs((x1-x0)*chordY-(y1-y0)*chordX)/std::max(chord,1.0f);
  const float d2=std::fabs((x2-x0)*chordY-(y2-y0)*chordX)/std::max(chord,1.0f);
  const int steps=std::max(2,std::min(24,2+int(std::max(d1,d2)/20.0f)));
  auto& pts=out.back().pts;
  for(int i=1;i<=steps;++i){const float t=float(i)/steps,mt=1.0f-t;const float px=mt*mt*mt*x0+3*mt*mt*t*x1+3*mt*t*t*x2+t*t*t*x3;const float py=mt*mt*mt*y0+3*mt*mt*t*y1+3*mt*t*t*y2+t*t*t*y3;pts.push_back({px,py,true});}
  x=x3;y=y3;
}
}  // namespace

bool CffFont::parsePrivateDict() {
  localSubrsInfo_={}; localSubrs_={};
  if(!privateDict_.valid()) return true;
  if(privateDict_.len>64u*1024u){lastError_="CFF Private DICT too large";return false;}
  std::vector<uint8_t> bytes(privateDict_.len);
  if(!readAt(privateDict_.off,bytes.data(),privateDict_.len)){lastError_="failed to read CFF Private DICT";return false;}
  int32_t stack[48]; size_t sp=0,pos=0; int32_t subrsRel=-1;
  while(pos<bytes.size()){
    const uint8_t b0=bytes[pos];
    if(b0>=32||b0==28||b0==29){++pos;int32_t v=0;
      if(b0>=32&&b0<=246)v=int32_t(b0)-139;
      else if(b0>=247&&b0<=250){if(pos>=bytes.size())return false;v=(int32_t(b0)-247)*256+bytes[pos++]+108;}
      else if(b0>=251&&b0<=254){if(pos>=bytes.size())return false;v=-(int32_t(b0)-251)*256-bytes[pos++]-108;}
      else if(b0==28){if(bytes.size()-pos<2)return false;v=int16_t(rd16(bytes.data()+pos));pos+=2;}
      else if(b0==29){if(bytes.size()-pos<4)return false;v=int32_t(rd32(bytes.data()+pos));pos+=4;}
      else return false;
      if(sp>=48)return false;stack[sp++]=v;continue;
    }
    ++pos;uint16_t op=b0;if(b0==12){if(pos>=bytes.size())return false;op=uint16_t(0x0c00|bytes[pos++]);}
    if(op==19){if(sp!=1||stack[0]<0){lastError_="invalid CFF local Subrs offset";return false;}subrsRel=stack[0];}
    sp=0;
  }
  if(subrsRel>=0){const uint64_t abs=uint64_t(privateDict_.off)+uint32_t(subrsRel);if(abs<cff_.off||abs>=uint64_t(cff_.off)+cff_.len){lastError_="CFF local Subrs outside table";return false;}if(!parseIndex(uint32_t(abs-cff_.off),localSubrsInfo_,nullptr)){lastError_="invalid CFF local Subr INDEX";return false;}localSubrs_=localSubrsInfo_.whole;}
  return true;
}

bool CffFont::executeType2(Slice code,std::vector<Contour>& out,int depth,float& x,float& y,uint32_t& stemCount) const {
  static constexpr uint32_t kMaxCode=256u*1024u;
  static constexpr uint32_t kMaxActiveBytes=512u*1024u;
  static constexpr int kMaxFrames=17;
  if(depth>16||!code.valid()||code.len>kMaxCode){lastError_="CFF Type2 recursion or CharString limit exceeded";return false;}
  struct Frame{uint32_t base=0,len=0,pos=0;};
  Frame frames[kMaxFrames];int frameCount=0;uint32_t used=0;
  float stack[48];size_t sp=0;
  auto pushFrame=[&](Slice slice)->bool{
    if(frameCount>=kMaxFrames||!slice.valid()||slice.len>kMaxCode||slice.len>kMaxActiveBytes-used){lastError_="CFF Type2 active bytecode limit exceeded";return false;}
    const uint32_t need=used+slice.len;
    if(need>type2ScratchCap_){auto*p=static_cast<uint8_t*>(reallocPsramFirst(type2Scratch_,need));if(!p){lastError_="CFF Type2 PSRAM scratch OOM";return false;}type2Scratch_=p;type2ScratchCap_=need;}
    if(!readAt(slice.off,type2Scratch_+used,slice.len)){lastError_="failed to read CFF Type2 CharString";return false;}
    frames[frameCount++]={used,slice.len,0};used=need;return true;
  };
  if(!pushFrame(code))return false;
  while(frameCount>0){
    Frame& frame=frames[frameCount-1];
    if(frame.pos>=frame.len){used=frame.base;--frameCount;continue;}
    uint8_t* bytes=type2Scratch_+frame.base;size_t pos=frame.pos;const uint8_t b0=bytes[pos];
    if(b0==28||b0==255||b0>=32){float value=0;if(!type2Number(bytes,frame.len,pos,value)||sp>=48){lastError_="malformed CFF Type2 operand";return false;}stack[sp++]=value;frame.pos=uint32_t(pos);continue;}
    ++pos;uint16_t op=b0;if(b0==12){if(pos>=frame.len){lastError_="truncated CFF Type2 escape";return false;}op=uint16_t(0x0c00|bytes[pos++]);}frame.pos=uint32_t(pos);
    auto clear=[&](){sp=0;};
    auto stem=[&](){stemCount+=uint32_t((sp-(sp&1u))/2u);clear();};
    switch(op){
      case 1:case 3:case 18:case 23: stem();break;
      case 19:case 20:{if(sp)stem();const size_t mask=(stemCount+7u)/8u;if(frame.len-frame.pos<mask){lastError_="truncated CFF hint mask";return false;}frame.pos+=uint32_t(mask);break;}
      case 4: if(!sp)return false;y+=stack[sp-1];moveTo(out,x,y);clear();break;
      case 21: if(sp<2)return false;x+=stack[sp-2];y+=stack[sp-1];moveTo(out,x,y);clear();break;
      case 22: if(!sp)return false;x+=stack[sp-1];moveTo(out,x,y);clear();break;
      case 5: if(sp<2||(sp&1u))return false;for(size_t i=0;i<sp;i+=2){x+=stack[i];y+=stack[i+1];lineTo(out,x,y);}clear();break;
      case 6:case 7:{bool horiz=op==6;for(size_t i=0;i<sp;++i){if(horiz)x+=stack[i];else y+=stack[i];lineTo(out,x,y);horiz=!horiz;}clear();break;}
      case 8: if(sp<6||sp%6)return false;for(size_t i=0;i<sp;i+=6)cubicTo(out,x,y,stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]);clear();break;
      case 24:{if(sp<8||(sp-2)%6)return false;size_t i=0;for(;i+2<sp;i+=6)cubicTo(out,x,y,stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]);x+=stack[i];y+=stack[i+1];lineTo(out,x,y);clear();break;}
      case 25:{if(sp<8||(sp-6)%2)return false;size_t i=0;for(;i+6<sp;i+=2){x+=stack[i];y+=stack[i+1];lineTo(out,x,y);}cubicTo(out,x,y,stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]);clear();break;}
      case 26:{size_t i=0;float dx1=0;if(sp&1u)dx1=stack[i++];if(sp-i<4||(sp-i)%4)return false;for(;i<sp;i+=4){cubicTo(out,x,y,dx1,stack[i],stack[i+1],stack[i+2],0,stack[i+3]);dx1=0;}clear();break;}
      case 27:{size_t i=0;float dy1=0;if(sp&1u)dy1=stack[i++];if(sp-i<4||(sp-i)%4)return false;for(;i<sp;i+=4){cubicTo(out,x,y,stack[i],dy1,stack[i+1],stack[i+2],stack[i+3],0);dy1=0;}clear();break;}
      case 30:case 31:{if(sp<4)return false;bool hf=op==31;size_t i=0;while(sp-i>=4){const bool last=sp-i==5||sp-i==4;float a=stack[i],b=stack[i+1],c=stack[i+2],d=stack[i+3],extra=0;if(last&&sp-i==5)extra=stack[i+4];if(hf)cubicTo(out,x,y,a,0,b,c,extra,d);else cubicTo(out,x,y,0,a,b,c,d,extra);i+=(last&&sp-i==5)?5:4;hf=!hf;}if(i!=sp)return false;clear();break;}
      case 10:case 29:{if(!sp)return false;const int raw=int(std::lround(stack[--sp]));const IndexInfo&idx=op==10?localSubrsInfo_:globalSubrsInfo_;if(!idx.count){lastError_="CFF Type2 references missing subroutine INDEX";return false;}const int biased=raw+subrBias(idx.count);if(biased<0||biased>=idx.count){lastError_="CFF Type2 subroutine index out of range";return false;}Slice sub;if(!indexObject(idx,uint16_t(biased),sub)||!pushFrame(sub))return false;break;}
      case 11: used=frame.base;--frameCount;break;
      case 14: clear();return true;
      case 0x0c22: if(sp!=7)return false;cubicTo(out,x,y,stack[0],0,stack[1],stack[2],stack[3],0);cubicTo(out,x,y,stack[4],0,stack[5],-stack[2],stack[6],0);clear();break;
      case 0x0c23: if(sp!=13)return false;cubicTo(out,x,y,stack[0],stack[1],stack[2],stack[3],stack[4],stack[5]);cubicTo(out,x,y,stack[6],stack[7],stack[8],stack[9],stack[10],stack[11]);clear();break;
      case 0x0c24: if(sp!=9)return false;cubicTo(out,x,y,stack[0],stack[1],stack[2],stack[3],stack[4],0);cubicTo(out,x,y,stack[5],0,stack[6],stack[7],stack[8],-(stack[1]+stack[3]+stack[7]));clear();break;
      case 0x0c25:{if(sp!=11)return false;const float dx=stack[0]+stack[2]+stack[4]+stack[6]+stack[8],dy=stack[1]+stack[3]+stack[5]+stack[7]+stack[9];const bool xd=std::fabs(dx)>std::fabs(dy);cubicTo(out,x,y,stack[0],stack[1],stack[2],stack[3],stack[4],stack[5]);cubicTo(out,x,y,stack[6],stack[7],stack[8],stack[9],xd?stack[10]:-dx,xd?-dy:stack[10]);clear();break;}
      default:lastError_="unsupported CFF Type2 operator";return false;
    }
  }
  return true;
}

bool CffFont::collectGlyph(uint16_t gid,std::vector<Contour>& out) const {
  if(!ready_||gid>=glyphCount_)return false;Slice glyph;if(!indexObject(charStringsInfo_,gid,glyph)){lastError_="failed to locate CFF glyph CharString";return false;}float x=0,y=0;uint32_t stems=0;if(!executeType2(glyph,out,0,x,y,stems))return false;lastError_="ok";return true;
}
} // namespace ttf

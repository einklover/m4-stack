#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>
#include "CffReader.h"

namespace {
void be16(std::vector<uint8_t>& v,uint16_t x){v.push_back(uint8_t(x>>8));v.push_back(uint8_t(x));}
void be32(std::vector<uint8_t>& v,uint32_t x){v.push_back(uint8_t(x>>24));v.push_back(uint8_t(x>>16));v.push_back(uint8_t(x>>8));v.push_back(uint8_t(x));}
void put16(std::vector<uint8_t>& v,size_t o,uint16_t x){v[o]=uint8_t(x>>8);v[o+1]=uint8_t(x);}
void put32(std::vector<uint8_t>& v,size_t o,uint32_t x){v[o]=uint8_t(x>>24);v[o+1]=uint8_t(x>>16);v[o+2]=uint8_t(x>>8);v[o+3]=uint8_t(x);}
void dictInt(std::vector<uint8_t>& out,int32_t n){out.push_back(29);be32(out,uint32_t(n));}
uint8_t t2(int n){assert(n>=-107&&n<=107);return uint8_t(n+139);}
void addIndex(std::vector<uint8_t>& c,const std::vector<std::vector<uint8_t>>& objects){
  be16(c,uint16_t(objects.size())); if(objects.empty())return;
  c.push_back(2); uint16_t off=1; be16(c,off);
  for(const auto& o:objects){off=uint16_t(off+o.size());be16(c,off);}
  for(const auto& o:objects)c.insert(c.end(),o.begin(),o.end());
}
std::vector<uint8_t> squareSubr(int width){
  return {t2(width),t2(0),t2(0),t2(100),t2(-width),t2(0),t2(0),t2(-100),5,11};
}
std::vector<uint8_t> glyphCallsLocalSubr(){return {t2(0),t2(0),21,t2(-107),10,14};}

std::vector<uint8_t> makePlainCff(){
  std::vector<uint8_t> c={1,0,4,2}; addIndex(c,{{'M','4'}});
  std::vector<uint8_t> top;
  const size_t csPatch=top.size();dictInt(top,0);top.push_back(17);
  dictInt(top,2);const size_t privatePatch=top.size();dictInt(top,0);top.push_back(18);
  const size_t topStart=c.size();addIndex(c,{top});addIndex(c,{});addIndex(c,{});
  const uint32_t cs=uint32_t(c.size());addIndex(c,{{14},glyphCallsLocalSubr()});
  const uint32_t priv=uint32_t(c.size());c.push_back(t2(2));c.push_back(19);addIndex(c,{squareSubr(100)});
  const size_t dictAbs=topStart+7;
  put32(c,dictAbs+csPatch+1,cs);put32(c,dictAbs+privatePatch+1,priv);
  return c;
}

std::vector<uint8_t> makeCidCff(bool format3){
  std::vector<uint8_t> c={1,0,4,2}; addIndex(c,{{'C','I','D'}});
  std::vector<uint8_t> top;
  dictInt(top,0);dictInt(top,1);dictInt(top,0);top.push_back(12);top.push_back(30); // ROS
  const size_t csPatch=top.size();dictInt(top,0);top.push_back(17);
  const size_t fdArrayPatch=top.size();dictInt(top,0);top.push_back(12);top.push_back(36);
  const size_t fdSelectPatch=top.size();dictInt(top,0);top.push_back(12);top.push_back(37);
  const size_t topStart=c.size();addIndex(c,{top});addIndex(c,{});addIndex(c,{});

  const uint32_t csRel=uint32_t(c.size());
  addIndex(c,{{14},glyphCallsLocalSubr(),glyphCallsLocalSubr()});

  const uint32_t fdArrayRel=uint32_t(c.size());
  std::vector<uint8_t> fd0,fd1;
  dictInt(fd0,2);const size_t fd0PrivatePatch=fd0.size();dictInt(fd0,0);fd0.push_back(18);
  dictInt(fd1,2);const size_t fd1PrivatePatch=fd1.size();dictInt(fd1,0);fd1.push_back(18);
  addIndex(c,{fd0,fd1});
  const size_t fdDataStart=size_t(fdArrayRel)+9; // count + offSize + 3x uint16 offsets

  const uint32_t fdSelectRel=uint32_t(c.size());
  if(format3){
    c.push_back(3);be16(c,2); // ranges: [0,2)->FD0, [2,3)->FD1
    be16(c,0);c.push_back(0);be16(c,2);c.push_back(1);be16(c,3);
  }else{
    c.push_back(0);c.push_back(0);c.push_back(0);c.push_back(1); // gid 0,1 -> FD0; gid2 -> FD1
  }

  const uint32_t private0Rel=uint32_t(c.size());
  c.push_back(t2(2));c.push_back(19);addIndex(c,{squareSubr(100)});
  const uint32_t private1Rel=uint32_t(c.size());
  c.push_back(t2(2));c.push_back(19);addIndex(c,{squareSubr(50)});

  const size_t topData=size_t(topStart)+7;
  put32(c,topData+csPatch+1,csRel);
  put32(c,topData+fdArrayPatch+1,fdArrayRel);
  put32(c,topData+fdSelectPatch+1,fdSelectRel);
  put32(c,fdDataStart+fd0PrivatePatch+1,private0Rel);
  put32(c,fdDataStart+fd0.size()+fd1PrivatePatch+1,private1Rel);
  return c;
}

void addTable(std::vector<uint8_t>& f,size_t rec,const char* tag,const std::vector<uint8_t>& data){
  while(f.size()&3u)f.push_back(0);const uint32_t off=uint32_t(f.size());
  std::memcpy(f.data()+rec,tag,4);put32(f,rec+8,off);put32(f,rec+12,uint32_t(data.size()));
  f.insert(f.end(),data.begin(),data.end());
}
std::vector<uint8_t> makeOtf(const std::vector<uint8_t>& cff,uint16_t glyphs,const std::vector<std::pair<uint32_t,uint16_t>>& cmapEntries){
  constexpr int tables=6;std::vector<uint8_t> f(12+tables*16,0);put32(f,0,0x4f54544f);put16(f,4,tables);size_t r=12;
  addTable(f,r,"CFF ",cff);r+=16;
  std::vector<uint8_t> head(54);put16(head,18,1000);put16(head,42,800);addTable(f,r,"head",head);r+=16;
  std::vector<uint8_t> maxp(6);put32(maxp,0,0x00005000);put16(maxp,4,glyphs);addTable(f,r,"maxp",maxp);r+=16;
  std::vector<uint8_t> hhea(36);put16(hhea,4,800);put16(hhea,6,uint16_t(-200));put16(hhea,8,100);put16(hhea,34,glyphs);addTable(f,r,"hhea",hhea);r+=16;
  std::vector<uint8_t> hmtx;for(uint16_t gid=0;gid<glyphs;++gid){be16(hmtx,gid?600:500);be16(hmtx,gid?10:0);}addTable(f,r,"hmtx",hmtx);r+=16;
  std::vector<uint8_t> cm;be16(cm,0);be16(cm,1);be16(cm,3);be16(cm,10);be32(cm,12);
  be16(cm,12);be16(cm,0);be32(cm,uint32_t(16+cmapEntries.size()*12));be32(cm,0);be32(cm,uint32_t(cmapEntries.size()));
  for(const auto& e:cmapEntries){be32(cm,e.first);be32(cm,e.first);be32(cm,e.second);}addTable(f,r,"cmap",cm);
  return f;
}

class VectorStream final:public ttf::TtfStream{
 public:explicit VectorStream(std::vector<uint8_t> bytes):bytes_(std::move(bytes)){}
  uint32_t size()const override{return uint32_t(bytes_.size());}
  bool seek(uint32_t p)override{if(p>bytes_.size())return false;pos_=p;return true;}
  uint32_t read(void* dst,uint32_t n)override{const uint32_t take=std::min<uint32_t>(n,uint32_t(bytes_.size()-pos_));if(take)std::memcpy(dst,bytes_.data()+pos_,take);pos_+=take;return take;}
 private:std::vector<uint8_t> bytes_;uint32_t pos_=0;
};

bool hasInk(const ttf::GlyphBitmap& gb){for(uint16_t i=0;i<gb.packedLen;++i)if(gb.data[i])return true;return false;}
void testPlain(){
  VectorStream s(makeOtf(makePlainCff(),2,{{0x41,1}}));ttf::CffFont f;assert(f.init(s));assert(!f.isCidKeyed());
  uint16_t gid=0;assert(f.findGlyph('A',gid)&&gid==1);ttf::GlyphBitmap gb;assert(f.rasterize(gid,100,gb));
  assert(gb.width==10&&gb.height==10&&gb.advance==60&&gb.packedLen==25&&hasInk(gb));
}
void testCid(bool format3){
  VectorStream s(makeOtf(makeCidCff(format3),3,{{0x41,1},{0x42,2}}));ttf::CffFont f;
  if(!f.init(s)){std::cerr<<"CID init failed: "<<f.lastError()<<'\n';std::abort();}
  assert(f.isCidKeyed()&&f.fdCount()==2);
  uint16_t a=0,b=0;assert(f.findGlyph('A',a)&&a==1);assert(f.findGlyph('B',b)&&b==2);
  ttf::GlyphBitmap ga,gb;assert(f.rasterize(a,100,ga));const int aw=ga.width,ah=ga.height,aa=ga.advance;const bool ai=hasInk(ga);
  // rasterize() reuses packed scratch, so inspect A before rendering B.
  assert(aw==10&&ah==10&&aa==60&&ai);
  assert(f.rasterize(b,100,gb));assert(gb.width==5&&gb.height==10&&gb.advance==60&&hasInk(gb));
}
} // namespace

int main(){
  testPlain();testCid(false);testCid(true);
  std::cout<<"CFF1 plain + CID FDSelect(0/3) + per-FD LocalSubrs + 2-bit raster OK\n";
  return 0;
}

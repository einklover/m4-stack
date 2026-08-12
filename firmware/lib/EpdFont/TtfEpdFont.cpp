#include "TtfEpdFont.h"

#include <HardwareSerial.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

extern void m4AppendFontDiagnostic(const char* line);

namespace {
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void putBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8); p[3] = static_cast<uint8_t>(v);
}
uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

class SdTtfStream : public ttf::TtfStream {
 public:
  bool open(const String& path) {
    close(); traceOps_ = 0;
    const bool ok = SdMan.openFileForRead("TtfFont", path.c_str(), file_);
    char line[256];
    snprintf(line, sizeof(line), "stream_open path=%s ok=%d size=%llu", path.c_str(), ok ? 1 : 0,
             static_cast<unsigned long long>(ok ? file_.fileSize() : 0));
    m4AppendFontDiagnostic(line); return ok;
  }
  void close() { if (file_.isOpen()) file_.close(); }
  uint32_t size() const override { return file_.isOpen() ? file_.fileSize() : 0; }
  bool seek(uint32_t pos) override {
    bool ok = file_.isOpen() && file_.seekSet(pos);
    for (uint8_t a = 0; !ok && a < 3 && file_.isOpen(); ++a) { delay(2); ok = file_.seekSet(pos); }
    bool seq = false;
    if (!ok && file_.isOpen() && pos <= file_.fileSize()) {
      file_.rewind(); uint8_t discard[512]; uint32_t remaining = pos;
      while (remaining) { const uint32_t want = std::min<uint32_t>(remaining, sizeof(discard)); const int got = file_.read(discard, want); if (got <= 0) break; remaining -= static_cast<uint32_t>(got); }
      ok = remaining == 0; seq = ok;
    }
    if (traceOps_ < 100) { char line[200]; snprintf(line, sizeof(line), "stream_seek pos=%lu ok=%d seq=%d cur=%llu size=%llu err=%u", static_cast<unsigned long>(pos), ok ? 1 : 0, seq ? 1 : 0, static_cast<unsigned long long>(file_.isOpen() ? file_.curPosition() : 0), static_cast<unsigned long long>(file_.isOpen() ? file_.fileSize() : 0), static_cast<unsigned>(file_.isOpen() ? file_.getError() : 0xff)); m4AppendFontDiagnostic(line); ++traceOps_; }
    return ok;
  }
  uint32_t read(void* dst, uint32_t n) override {
    if (!file_.isOpen()) return 0; auto* out = static_cast<uint8_t*>(dst); uint32_t total = 0; const uint64_t start = file_.curPosition();
    while (total < n) { int got = file_.read(out + total, n - total); for (uint8_t a = 0; got <= 0 && a < 3; ++a) { delay(2); got = file_.read(out + total, n - total); } if (got <= 0) break; total += static_cast<uint32_t>(got); }
    if (traceOps_ < 100) { char line[180]; snprintf(line, sizeof(line), "stream_read pos=%llu want=%lu got=%lu end=%llu", static_cast<unsigned long long>(start), static_cast<unsigned long>(n), static_cast<unsigned long>(total), static_cast<unsigned long long>(file_.curPosition())); m4AppendFontDiagnostic(line); ++traceOps_; }
    return total;
  }
 private: FsFile file_; uint8_t traceOps_ = 0;
};

class MemoryTtfStream : public ttf::TtfStream {
 public:
  MemoryTtfStream(const uint8_t* data, uint32_t size) : data_(data), size_(size) {}
  uint32_t size() const override { return size_; }
  bool seek(uint32_t pos) override { if (pos > size_) return false; pos_ = pos; return true; }
  uint32_t read(void* dst, uint32_t n) override { if (!data_ || !dst || pos_ >= size_) return 0; const uint32_t take = std::min(n, size_ - pos_); if (take) { std::memcpy(dst, data_ + pos_, take); pos_ += take; } return take; }
 private: const uint8_t* data_ = nullptr; uint32_t size_ = 0, pos_ = 0;
};

class GlyfOpenTypeStream : public ttf::TtfStream {
 public:
  explicit GlyfOpenTypeStream(SdTtfStream* source) : source_(source) {}
  ~GlyfOpenTypeStream() override { delete source_; }
  bool init() {
    if (!source_ || source_->size() < 12) return false; uint8_t hdr[12];
    if (!readSource(0, hdr, sizeof(hdr)) || std::memcmp(hdr, "OTTO", 4) != 0) return false;
    const uint16_t nt = be16(hdr + 4); if (!nt || nt > 64 || 12u + uint32_t(nt) * 16u > source_->size()) return false;
    bool head=false,maxp=false,loca=false,cmap=false,hhea=false,hmtx=false,glyf=false;
    for (uint16_t i=0;i<nt;++i) { uint8_t r[16]; if(!readSource(12u+uint32_t(i)*16u,r,16))return false; const uint32_t t=be32(r),o=be32(r+8),l=be32(r+12); if(o>source_->size()||l>source_->size()-o)return false; head|=t==0x68656164u;maxp|=t==0x6d617870u;loca|=t==0x6c6f6361u;cmap|=t==0x636d6170u;hhea|=t==0x68686561u;hmtx|=t==0x686d7478u;glyf|=t==0x676c7966u; }
    if (!(head&&maxp&&loca&&cmap&&hhea&&hmtx&&glyf)) return false; pos_=0; m4AppendFontDiagnostic("opentype_glyf_alias enabled"); return true;
  }
  uint32_t size() const override { return source_ ? source_->size() : 0; }
  bool seek(uint32_t p) override { if(!source_||p>source_->size())return false;pos_=p;return true; }
  uint32_t read(void* dst,uint32_t n) override { if(!source_||!dst||pos_>=source_->size())return 0;n=std::min(n,source_->size()-pos_);auto*out=static_cast<uint8_t*>(dst);uint32_t total=0;static constexpr uint8_t sig[4]={0,1,0,0};while(total<n&&pos_<4)out[total++]=sig[pos_++];if(total<n){if(!source_->seek(pos_))return total;uint32_t got=source_->read(out+total,n-total);pos_+=got;total+=got;}return total; }
 private:
  bool readSource(uint32_t o,void*d,uint32_t n){return o<=source_->size()&&n<=source_->size()-o&&source_->seek(o)&&source_->read(d,n)==n;}
  SdTtfStream* source_=nullptr; uint32_t pos_=0;
};

class TtcFaceStream : public ttf::TtfStream {
 public:
  explicit TtcFaceStream(SdTtfStream* source):source_(source){} ~TtcFaceStream() override{delete source_;}
  bool init(){if(!source_||source_->size()<16)return false;uint8_t h[12];if(!readSource(0,h,12)||std::memcmp(h,"ttcf",4)!=0)return false;uint32_t count=be32(h+8);if(!count||count>64||12u+count*4u>source_->size())return false;for(uint32_t i=0;i<count;++i){uint8_t b[4];if(!readSource(12u+i*4u,b,4))return false;if(buildFace(be32(b),i)){char line[160];snprintf(line,sizeof(line),"ttc_face_selected index=%lu faces=%lu virtual_size=%lu",static_cast<unsigned long>(i),static_cast<unsigned long>(count),static_cast<unsigned long>(size_));m4AppendFontDiagnostic(line);return true;}}return false;}
  uint32_t size() const override{return size_;} bool seek(uint32_t p) override{if(p>size_)return false;pos_=p;return true;}
  uint32_t read(void*dst,uint32_t n) override{if(!dst||pos_>=size_)return 0;auto*out=static_cast<uint8_t*>(dst);uint32_t total=0;n=std::min(n,size_-pos_);while(total<n){if(pos_<directory_.size()){uint32_t take=std::min<uint32_t>(n-total,uint32_t(directory_.size())-pos_);std::memcpy(out+total,directory_.data()+pos_,take);pos_+=take;total+=take;continue;}const TableMap*hit=nullptr;uint32_t next=size_;for(const auto&t:tables_){if(pos_>=t.virtualOff&&pos_<t.virtualOff+t.len){hit=&t;break;}if(t.virtualOff>pos_)next=std::min(next,t.virtualOff);}if(hit){uint32_t d=pos_-hit->virtualOff,take=std::min<uint32_t>(n-total,hit->len-d);if(!source_->seek(hit->sourceOff+d))break;uint32_t got=source_->read(out+total,take);if(!got)break;pos_+=got;total+=got;continue;}uint32_t take=std::min<uint32_t>(n-total,next>pos_?next-pos_:1u);std::memset(out+total,0,take);pos_+=take;total+=take;}return total;}
 private:
  struct TableMap{uint32_t virtualOff=0,sourceOff=0,len=0;};
  bool readSource(uint32_t o,void*d,uint32_t n){return o<=source_->size()&&n<=source_->size()-o&&source_->seek(o)&&source_->read(d,n)==n;}
  bool buildFace(uint32_t faceOff,uint32_t faceIndex){if(faceOff>source_->size()||source_->size()-faceOff<12)return false;uint8_t fh[12];if(!readSource(faceOff,fh,12))return false;uint32_t sig=be32(fh);if(sig!=0x00010000u&&sig!=0x74727565u&&sig!=0x4f54544fu)return false;uint16_t nt=be16(fh+4);if(!nt||nt>64)return false;uint32_t db=12u+uint32_t(nt)*16u;if(db>source_->size()-faceOff)return false;std::vector<uint8_t>dir(db);if(!readSource(faceOff,dir.data(),db))return false;std::vector<TableMap>maps;maps.reserve(nt);bool head=false,maxp=false,loca=false,cmap=false,hhea=false,hmtx=false,glyf=false,cff=false,cff2=false;uint32_t vo=align4(db);for(uint16_t i=0;i<nt;++i){uint8_t*r=dir.data()+12u+uint32_t(i)*16u;uint32_t t=be32(r),so=be32(r+8),l=be32(r+12);if(so>source_->size()||l>source_->size()-so)return false;if(l&&vo>0xffffffffu-align4(l))return false;maps.push_back({vo,so,l});putBe32(r+8,vo);vo+=align4(l);head|=t==0x68656164u;maxp|=t==0x6d617870u;loca|=t==0x6c6f6361u;cmap|=t==0x636d6170u;hhea|=t==0x68686561u;hmtx|=t==0x686d7478u;glyf|=t==0x676c7966u;cff|=t==0x43464620u;cff2|=t==0x43464632u;}if(!(head&&maxp&&loca&&cmap&&hhea&&hmtx&&glyf)){if(sig==0x4f54544fu&&(cff||cff2)){char line[128];snprintf(line,sizeof(line),"collection_face index=%lu unsupported=%s",static_cast<unsigned long>(faceIndex),cff2?"CFF2":"CFF");m4AppendFontDiagnostic(line);}return false;}if(sig==0x4f54544fu)putBe32(dir.data(),0x00010000u);directory_.swap(dir);tables_.swap(maps);size_=vo;pos_=0;return true;}
  SdTtfStream*source_=nullptr;std::vector<uint8_t>directory_;std::vector<TableMap>tables_;uint32_t size_=0,pos_=0;
};

enum class OpenTypeKind:uint8_t{Glyf,Cff1,Cff2,Invalid};

OpenTypeKind probeSfntFace(SdTtfStream& s,uint32_t faceOff){
  if(faceOff>s.size()||s.size()-faceOff<12)return OpenTypeKind::Invalid;
  uint8_t h[12];if(!s.seek(faceOff)||s.read(h,12)!=12)return OpenTypeKind::Invalid;
  const uint32_t sig=be32(h);
  if(sig!=0x00010000u&&sig!=0x74727565u&&sig!=0x4f54544fu)return OpenTypeKind::Invalid;
  const uint16_t nt=be16(h+4);
  if(!nt||nt>128||uint64_t(faceOff)+12u+uint64_t(nt)*16u>s.size())return OpenTypeKind::Invalid;
  bool head=false,maxp=false,loca=false,cmap=false,hhea=false,hmtx=false,glyf=false,cff=false,cff2=false;
  for(uint16_t i=0;i<nt;++i){
    uint8_t r[16];if(!s.seek(faceOff+12u+uint32_t(i)*16u)||s.read(r,16)!=16)return OpenTypeKind::Invalid;
    const uint32_t t=be32(r),o=be32(r+8),l=be32(r+12);
    // TTC/OTC table offsets are absolute from the beginning of the collection.
    if(o>s.size()||l>s.size()-o)return OpenTypeKind::Invalid;
    head|=t==0x68656164u;maxp|=t==0x6d617870u;loca|=t==0x6c6f6361u;cmap|=t==0x636d6170u;
    hhea|=t==0x68686561u;hmtx|=t==0x686d7478u;glyf|=t==0x676c7966u;
    cff|=t==0x43464620u;cff2|=t==0x43464632u;
  }
  if(head&&maxp&&loca&&cmap&&hhea&&hmtx&&glyf)return OpenTypeKind::Glyf;
  if(sig==0x4f54544fu&&head&&maxp&&cmap&&hhea&&hmtx&&cff)return OpenTypeKind::Cff1;
  if(sig==0x4f54544fu&&cff2)return OpenTypeKind::Cff2;
  return OpenTypeKind::Invalid;
}
OpenTypeKind probeOpenType(SdTtfStream& s){return probeSfntFace(s,0);}

bool probeCollection(SdTtfStream& s,uint32_t& faceOff,OpenTypeKind& kind,bool& sawCff2){
  faceOff=0;kind=OpenTypeKind::Invalid;sawCff2=false;
  if(s.size()<16)return false;
  uint8_t h[12];if(!s.seek(0)||s.read(h,12)!=12||std::memcmp(h,"ttcf",4)!=0)return false;
  const uint32_t count=be32(h+8);
  if(!count||count>64||12u+uint64_t(count)*4u>s.size())return false;
  for(uint32_t i=0;i<count;++i){
    uint8_t b[4];if(!s.seek(12u+i*4u)||s.read(b,4)!=4)return false;
    const uint32_t off=be32(b);const OpenTypeKind candidate=probeSfntFace(s,off);
    if(candidate==OpenTypeKind::Cff2){sawCff2=true;continue;}
    if(candidate==OpenTypeKind::Cff1||candidate==OpenTypeKind::Glyf){
      faceOff=off;kind=candidate;
      char line[160];snprintf(line,sizeof(line),"collection_face_probe index=%lu offset=%lu backend=%s",static_cast<unsigned long>(i),static_cast<unsigned long>(off),candidate==OpenTypeKind::Cff1?"cff1":"glyf");m4AppendFontDiagnostic(line);
      return true;
    }
  }
  return false;
}

void* ttfAlloc(size_t n){
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
  if(psramFound())return ps_malloc(n);
#endif
  return malloc(n);
}
void ttfFree(void*p){free(p);} int clampMetric(int v,int lo,int hi){if(lo>hi)std::swap(lo,hi);return std::max(lo,std::min(hi,v));}
} // namespace

void* TtfEpdFont::ttfAlloc(size_t n){return ::ttfAlloc(n);} void TtfEpdFont::ttfFree(void*p){::ttfFree(p);}

const char* TtfEpdFont::backendError() const{return backend_==Backend::Cff1?cffFont_.lastError():font_.lastError();}
uint16_t TtfEpdFont::backendUnitsPerEm() const{return backend_==Backend::Cff1?cffFont_.unitsPerEm():font_.unitsPerEm();}
int32_t TtfEpdFont::backendBBoxYMax() const{return backend_==Backend::Cff1?cffFont_.fontBBoxYMax():font_.fontBBoxYMax();}
bool TtfEpdFont::backendFindGlyph(uint32_t cp,uint16_t&gid) const{return backend_==Backend::Cff1?cffFont_.findGlyph(cp,gid):font_.findGlyph(cp,gid);}
bool TtfEpdFont::backendRasterize(uint16_t gid,ttf::GlyphBitmap&out) const{return backend_==Backend::Cff1?cffFont_.rasterize(gid,sizePx_,out):font_.rasterize(gid,sizePx_,out);}
bool TtfEpdFont::backendPixelBox(uint16_t gid,int&x0,int&y0,int&x1,int&y1) const{return backend_==Backend::Cff1?cffFont_.glyphPixelBox(gid,sizePx_,x0,y0,x1,y1):font_.glyphPixelBox(gid,sizePx_,x0,y0,x1,y1);}
void TtfEpdFont::backendVMetrics(int32_t&a,int32_t&d,int32_t&g) const{if(backend_==Backend::Cff1)cffFont_.fontVMetrics(a,d,g);else font_.fontVMetrics(a,d,g);} void TtfEpdFont::backendClearScratch(){if(backend_==Backend::Cff1)cffFont_.clearScratch();else font_.clearScratch();}
const char* TtfEpdFont::lastError() const{return runtimeError_.length()?runtimeError_.c_str():backendError();}
bool TtfEpdFont::hasCodepoint(uint32_t cp) const{uint16_t gid=0;return valid_&&backendFindGlyph(cp,gid)&&gid!=0;}

bool TtfEpdFont::allocateEntries(){if(maxSlots_==0)maxSlots_=1;if(cacheBudget_==0)cacheBudget_=1;entries_=static_cast<Entry*>(ttfAlloc(sizeof(Entry)*maxSlots_));if(!entries_){runtimeError_="glyph cache metadata allocation failed";return false;}for(uint16_t i=0;i<maxSlots_;++i)new(&entries_[i])Entry();return true;}

bool TtfEpdFont::finishInit(const char* label){if(!stream_)return false;const bool ok=backend_==Backend::Cff1?cffFont_.init(*stream_,cffFaceOffset_):font_.init(*stream_);if(!ok){Serial.printf("[TTF] Invalid runtime font %s backend=%s: %s\n",label?label:"?",backend_==Backend::Cff1?"cff1":"glyf",backendError());return false;}const uint16_t upm=backendUnitsPerEm();if(!upm)return false;const float scale=float(sizePx_)/float(upm);int32_t asc=0,desc=0,gap=0;backendVMetrics(asc,desc,gap);const int rawAsc=int(std::lround(asc*scale)),rawDesc=int(std::lround(desc*scale)),gapPx=std::max(0,int(std::lround(gap*scale))),bboxTop=std::max(0,int(std::lround(backendBBoxYMax()*scale)));bool refValid=false;uint32_t refCp=0;int refTop=0,refBottom=0;static constexpr uint32_t samples[]={0x56FD,0x7530,0x4E2D,0x6C38,0x4E00,'H','M'};for(uint32_t cp:samples){uint16_t gid=0;if(!backendFindGlyph(cp,gid)||gid==0)continue;int x0=0,y0=0,x1=0,y1=0;if(!backendPixelBox(gid,x0,y0,x1,y1))continue;int top=std::max(0,-y0),bottom=std::max(0,y1),height=top+bottom;if(height<std::max(2,int(sizePx_)/2)||height>255)continue;refValid=true;refCp=cp;refTop=top;refBottom=bottom;break;}const int nominal=std::max(1,int(sizePx_));int ascPx=refValid?clampMetric(refTop,std::max(1,int(std::lround(nominal*.55f))),std::max(1,int(std::lround(nominal*1.10f)))):clampMetric(rawAsc,std::max(1,int(std::lround(nominal*.60f))),std::max(1,int(std::lround(nominal*1.10f))));int descMag=std::max(0,-rawDesc);if(refValid)descMag=std::max(descMag,refBottom);descMag=clampMetric(descMag,0,std::max(1,int(std::lround(nominal*.30f))));const int descPx=-descMag;const int clippedGap=std::min(gapPx,std::max(0,int(std::lround(nominal*.25f))));int line=std::max(ascPx+descMag+clippedGap,nominal);line=std::min(line,std::max(nominal,int(std::lround(nominal*1.35f))));line=std::max(1,std::min(255,line));data_.bitmap=nullptr;data_.glyph=nullptr;data_.intervals=nullptr;data_.intervalCount=0;data_.advanceY=uint8_t(line);data_.ascender=ascPx;data_.descender=descPx;data_.is2Bit=true;valid_=true;Serial.printf("[TTF] Loaded %s backend=%s @%upx upm=%u lineH=%u asc=%d desc=%d bboxTop=%d ref=U+%04X slots=%u budget=%u\n",label?label:"?",backend_==Backend::Cff1?"cff1":"glyf",sizePx_,upm,data_.advanceY,data_.ascender,data_.descender,bboxTop,static_cast<unsigned>(refValid?refCp:0),static_cast<unsigned>(maxSlots_),static_cast<unsigned>(cacheBudget_));return true;}

TtfEpdFont::TtfEpdFont(const String& path,uint16_t sizePx,uint16_t maxSlots,size_t budget):EpdFont(&data_),path_(path),sizePx_(sizePx),maxSlots_(maxSlots),cacheBudget_(budget){
#if defined(ESP32)
  mutex_=xSemaphoreCreateMutex();
#endif
  auto*sd=new(std::nothrow)SdTtfStream();if(!sd||!sd->open(path_)){runtimeError_="font file open failed";delete sd;return;}uint8_t magic[4]={};const bool have=sd->seek(0)&&sd->read(magic,4)==4;
  if(have&&std::memcmp(magic,"ttcf",4)==0){
    uint32_t faceOff=0;OpenTypeKind kind=OpenTypeKind::Invalid;bool sawCff2=false;
    if(!probeCollection(*sd,faceOff,kind,sawCff2)){runtimeError_=sawCff2?"font collection contains only unsupported CFF2 faces":"font collection contains no supported glyf/CFF1 face";delete sd;return;}
    if(kind==OpenTypeKind::Cff1){stream_=sd;backend_=Backend::Cff1;cffFaceOffset_=faceOff;m4AppendFontDiagnostic("collection_cff1_zero_copy enabled");}
    else{auto*c=new(std::nothrow)TtcFaceStream(sd);if(!c||!c->init()){runtimeError_="TrueType collection glyf adapter failed";delete c;if(!c)delete sd;return;}stream_=c;backend_=Backend::Glyf;}
  }
  else if(have&&std::memcmp(magic,"OTTO",4)==0){const OpenTypeKind kind=probeOpenType(*sd);if(kind==OpenTypeKind::Cff1){sd->seek(0);stream_=sd;backend_=Backend::Cff1;cffFaceOffset_=0;m4AppendFontDiagnostic("opentype_cff1_backend enabled");}else if(kind==OpenTypeKind::Glyf){auto*o=new(std::nothrow)GlyfOpenTypeStream(sd);if(!o||!o->init()){runtimeError_="OpenType glyf adapter failed";delete o;if(!o)delete sd;return;}stream_=o;backend_=Backend::Glyf;}else{runtimeError_=kind==OpenTypeKind::Cff2?"CFF2 variable OpenType is not supported yet":"invalid OpenType font";delete sd;return;}}
  else{sd->seek(0);stream_=sd;backend_=Backend::Glyf;}
  if(!finishInit(path_.c_str()))return;if(!allocateEntries()){valid_=false;return;}
}

TtfEpdFont::TtfEpdFont(const uint8_t*data,uint32_t dataSize,uint16_t sizePx,uint16_t maxSlots,size_t budget):EpdFont(&data_),path_("<flash>"),sizePx_(sizePx),maxSlots_(maxSlots),cacheBudget_(budget){
#if defined(ESP32)
  mutex_=xSemaphoreCreateMutex();
#endif
  if(!data||!dataSize){runtimeError_="empty embedded TTF";return;}stream_=new(std::nothrow)MemoryTtfStream(data,dataSize);backend_=Backend::Glyf;if(!stream_){runtimeError_="embedded font stream allocation failed";return;}if(!finishInit("<flash>"))return;if(!allocateEntries()){valid_=false;return;}
}

TtfEpdFont::~TtfEpdFont(){clearCaches();if(entries_){for(uint16_t i=0;i<maxSlots_;++i)entries_[i].~Entry();ttfFree(entries_);entries_=nullptr;}delete stream_;stream_=nullptr;
#if defined(ESP32)
  if(mutex_){vSemaphoreDelete(mutex_);mutex_=nullptr;}
#endif
}
void TtfEpdFont::evictSlot(int slot) const{if(!entries_||slot<0||slot>=int(maxSlots_))return;if(entries_[slot].bitmap){ttfFree(entries_[slot].bitmap);cacheBytes_-=entries_[slot].bitmapSize;}entries_[slot]=Entry{};}
void TtfEpdFont::clearCaches(){
#if defined(ESP32)
  if(mutex_)xSemaphoreTake(mutex_,portMAX_DELAY);
#endif
  if(entries_)for(uint16_t i=0;i<maxSlots_;++i)evictSlot(i);backendClearScratch();
#if defined(ESP32)
  if(mutex_)xSemaphoreGive(mutex_);
#endif
}
int TtfEpdFont::ensureGlyph(uint32_t cp) const{if(!valid_||!entries_)return-1;for(uint16_t i=0;i<maxSlots_;++i)if(entries_[i].cp==cp){entries_[i].lastAccess=++accessCounter_;return i;}uint16_t gid=0;if(!backendFindGlyph(cp,gid))return-1;if(gid==0&&cp!='?')return ensureGlyph('?');ttf::GlyphBitmap gb;if(!backendRasterize(gid,gb)){if(gid==0||!backendRasterize(0,gb))return-1;}int slot=-1;uint32_t minAccess=0xffffffffu;for(uint16_t i=0;i<maxSlots_;++i){if(entries_[i].cp==0xffffffffu){slot=i;break;}if(entries_[i].lastAccess<minAccess){minAccess=entries_[i].lastAccess;slot=i;}}if(slot<0)return-1;evictSlot(slot);uint8_t*bmp=nullptr;const uint32_t len=gb.packedLen;if(len&&gb.data){bmp=static_cast<uint8_t*>(ttfAlloc(len));if(!bmp)return-1;std::memcpy(bmp,gb.data,len);}entries_[slot].cp=cp;entries_[slot].lastAccess=++accessCounter_;entries_[slot].glyph.width=uint8_t(gb.width);entries_[slot].glyph.height=uint8_t(gb.height);entries_[slot].glyph.advanceX=uint8_t(std::max(0,std::min(255,int(gb.advance))));entries_[slot].glyph.left=gb.xoff;entries_[slot].glyph.top=gb.yoff;entries_[slot].glyph.dataLength=len;entries_[slot].glyph.dataOffset=cp;entries_[slot].bitmap=bmp;entries_[slot].bitmapSize=len;cacheBytes_+=len;while(cacheBytes_>cacheBudget_){int victim=-1;uint32_t la=0xffffffffu;for(uint16_t i=0;i<maxSlots_;++i){if(int(i)==slot||entries_[i].cp==0xffffffffu)continue;if(entries_[i].lastAccess<la){la=entries_[i].lastAccess;victim=i;}}if(victim<0)break;evictSlot(victim);}return slot;}
const EpdGlyph* TtfEpdFont::getGlyph(uint32_t cp,const EpdFontStyles::Style style) const{(void)style;
#if defined(ESP32)
  if(mutex_)xSemaphoreTake(mutex_,portMAX_DELAY);
#endif
  int slot=ensureGlyph(cp);
#if defined(ESP32)
  if(mutex_)xSemaphoreGive(mutex_);
#endif
  return slot<0?nullptr:&entries_[slot].glyph;}
const uint8_t* TtfEpdFont::loadGlyphBitmap(const EpdGlyph*glyph,uint8_t*buffer,const EpdFontStyles::Style style) const{(void)style;if(!glyph)return nullptr;uint32_t cp=glyph->dataOffset;
#if defined(ESP32)
  if(mutex_)xSemaphoreTake(mutex_,portMAX_DELAY);
#endif
  int slot=ensureGlyph(cp);const uint8_t*result=nullptr;if(slot>=0&&entries_[slot].bitmap&&entries_[slot].bitmapSize){if(buffer){std::memcpy(buffer,entries_[slot].bitmap,entries_[slot].bitmapSize);result=buffer;}else result=entries_[slot].bitmap;}
#if defined(ESP32)
  if(mutex_)xSemaphoreGive(mutex_);
#endif
  return result;}

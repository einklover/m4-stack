// CffReader — streamed Compact Font Format 1 parser for OpenType/CFF faces.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "TtfReader.h"
namespace ttf {
class CffFont {
 public:
  struct Slice { uint32_t off=0,len=0; bool valid() const { return len!=0; } };
  CffFont()=default; CffFont(const CffFont&)=delete; CffFont& operator=(const CffFont&)=delete; ~CffFont();
  bool init(TtfStream& stream,uint32_t faceOffset=0);
  bool ready() const{return ready_;} const char* lastError() const{return lastError_;}
  uint16_t glyphCount() const{return glyphCount_;} uint16_t unitsPerEm() const{return unitsPerEm_;}
  int32_t fontBBoxYMax() const{return bboxYMax_;}
  Slice cffTable() const{return cff_;} Slice charStringsIndex() const{return charStrings_;}
  Slice globalSubrsIndex() const{return globalSubrs_;} Slice localSubrsIndex() const{return localSubrs_;}
  Slice privateDict() const{return privateDict_;}
  bool indexObject(Slice index,uint16_t item,Slice& object) const;
  // Debug/test outline path. Runtime rasterization bypasses this STL structure
  // and writes flattened edges directly to reusable PSRAM scratch.
  bool collectGlyph(uint16_t gid,std::vector<Contour>& out) const;
  bool findGlyph(uint32_t cp,uint16_t& gid) const;
  bool glyphHMetrics(uint16_t gid,int32_t& advUnits,int32_t& lsbUnits) const;
  void fontVMetrics(int32_t& ascUnits,int32_t& descUnits,int32_t& gapUnits) const;
  bool glyphPixelBox(uint16_t gid,uint16_t sizePx,int& x0,int& y0,int& x1,int& y1) const;
  bool rasterize(uint16_t gid,uint16_t sizePx,GlyphBitmap& out);
  void clearScratch();
 private:
  struct IndexInfo { Slice whole; uint16_t count=0; uint8_t offSize=0; uint32_t offsetsOff=0,dataOff=0; };
  struct Table { uint32_t off=0,len=0; bool present=false; };
  struct Intersection { float x=0; int8_t sign=0; };
  struct Edge { float x0=0,y0=0,x1=0,y1=0; };
  struct EdgeBuildState {
    bool open=false,haveBounds=false;
    float startX=0,startY=0,lastX=0,lastY=0;
    float minX=0,maxX=0,minY=0,maxY=0;
    uint32_t count=0;
  };
  bool readAt(uint32_t off,void* dst,uint32_t n) const; bool parseIndex(uint32_t relOff,IndexInfo& out,uint32_t* nextRel=nullptr) const;
  bool indexObject(const IndexInfo& index,uint16_t item,Slice& object) const; bool parseTopDict(Slice dict); bool parsePrivateDict();
  bool initSfntMetrics(uint32_t faceOffset,uint16_t numTables); bool initCmap(); bool readOffset(uint32_t absOff,uint8_t offSize,uint32_t& value) const;
  bool executeType2(Slice code,std::vector<Contour>* debugOut,EdgeBuildState* edgeOut,int depth,float& x,float& y,uint32_t& stemCount) const;
  bool collectEdges(uint16_t gid,EdgeBuildState& state) const;
  bool edgeMove(EdgeBuildState& state,float x,float y) const;
  bool edgeLine(EdgeBuildState& state,float x,float y) const;
  bool edgeClose(EdgeBuildState& state) const;
  bool appendEdge(EdgeBuildState& state,float x0,float y0,float x1,float y1) const;
  TtfStream* stream_=nullptr; uint32_t fileSize_=0; bool ready_=false; mutable const char* lastError_="not initialized";
  Slice cff_; IndexInfo charStringsInfo_,globalSubrsInfo_,localSubrsInfo_; Slice charStrings_,globalSubrs_,localSubrs_,privateDict_; uint16_t glyphCount_=0;
  Table head_,cmap_,hhea_,hmtx_,maxp_; uint16_t unitsPerEm_=0; int32_t ascender_=0,descender_=0,lineGap_=0,bboxYMax_=0; uint16_t numHMetrics_=0;
  uint8_t* cmapData_=nullptr; uint32_t cmapLen_=0,cmapScratchCap_=0; bool cmapIs12_=false; uint32_t cmapGroups_=0;
  mutable uint8_t* type2Scratch_=nullptr; mutable uint32_t type2ScratchCap_=0;
  mutable Edge* edgeScratch_=nullptr; mutable uint32_t edgeScratchCap_=0;
  uint8_t* covScratch_=nullptr; uint32_t covScratchCap_=0;
  uint8_t* packedScratch_=nullptr; uint32_t packedScratchCap_=0;
  Intersection* intersectionScratch_=nullptr; uint32_t intersectionScratchCap_=0;
};
} // namespace ttf

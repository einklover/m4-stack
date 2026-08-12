#include "CffReader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace ttf {
namespace {

uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t rd32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

bool type2Number(const uint8_t* data, size_t len, size_t& pos, float& out) {
  if (pos >= len) return false;
  const uint8_t b0 = data[pos++];
  if (b0 >= 32 && b0 <= 246) {
    out = static_cast<float>(static_cast<int>(b0) - 139);
    return true;
  }
  if (b0 >= 247 && b0 <= 250) {
    if (pos >= len) return false;
    out = static_cast<float>((b0 - 247) * 256 + data[pos++] + 108);
    return true;
  }
  if (b0 >= 251 && b0 <= 254) {
    if (pos >= len) return false;
    out = static_cast<float>(-(static_cast<int>(b0) - 251) * 256 - data[pos++] - 108);
    return true;
  }
  if (b0 == 28) {
    if (len - pos < 2) return false;
    out = static_cast<float>(static_cast<int16_t>(rd16(data + pos)));
    pos += 2;
    return true;
  }
  if (b0 == 255) {
    if (len - pos < 4) return false;
    const int32_t raw = static_cast<int32_t>(rd32(data + pos));
    pos += 4;
    out = static_cast<float>(raw) / 65536.0f;
    return true;
  }
  return false;
}

int subrBias(uint16_t count) {
  if (count < 1240) return 107;
  if (count < 33900) return 1131;
  return 32768;
}

void ensureContour(std::vector<Contour>& out, float x, float y) {
  if (out.empty() || out.back().pts.empty()) {
    Contour c;
    c.pts.push_back({x, y, true});
    out.push_back(std::move(c));
  }
}

void moveTo(std::vector<Contour>& out, float x, float y) {
  Contour c;
  c.pts.push_back({x, y, true});
  out.push_back(std::move(c));
}

void lineTo(std::vector<Contour>& out, float x, float y) {
  ensureContour(out, x, y);
  auto& pts = out.back().pts;
  if (pts.empty() || std::fabs(pts.back().x - x) > 1e-6f || std::fabs(pts.back().y - y) > 1e-6f)
    pts.push_back({x, y, true});
}

void cubicTo(std::vector<Contour>& out, float& x, float& y,
             float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
  ensureContour(out, x, y);
  const float x0 = x, y0 = y;
  const float x1 = x0 + dx1, y1 = y0 + dy1;
  const float x2 = x1 + dx2, y2 = y1 + dy2;
  const float x3 = x2 + dx3, y3 = y2 + dy3;

  // Bound flattening in font units. The deviation metric is scale-independent;
  // 8..24 segments is enough for typical 1000/2048 UPM fonts at M4 sizes.
  const float chordX = x3 - x0, chordY = y3 - y0;
  const float chord = std::sqrt(chordX * chordX + chordY * chordY);
  const float d1 = std::fabs((x1 - x0) * chordY - (y1 - y0) * chordX) / std::max(chord, 1.0f);
  const float d2 = std::fabs((x2 - x0) * chordY - (y2 - y0) * chordX) / std::max(chord, 1.0f);
  const int steps = std::max(2, std::min(24, 2 + static_cast<int>(std::max(d1, d2) / 20.0f)));
  auto& pts = out.back().pts;
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / steps;
    const float mt = 1.0f - t;
    const float px = mt * mt * mt * x0 + 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t * t * t * x3;
    const float py = mt * mt * mt * y0 + 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t * y3;
    pts.push_back({px, py, true});
  }
  x = x3;
  y = y3;
}

}  // namespace

bool CffFont::parsePrivateDict() {
  localSubrsInfo_ = IndexInfo{};
  localSubrs_ = Slice{};
  if (!privateDict_.valid()) return true;
  if (privateDict_.len > 64u * 1024u) {
    lastError_ = "CFF Private DICT too large";
    return false;
  }
  std::vector<uint8_t> bytes(privateDict_.len);
  if (!readAt(privateDict_.off, bytes.data(), privateDict_.len)) {
    lastError_ = "failed to read CFF Private DICT";
    return false;
  }

  std::vector<int32_t> stack;
  size_t pos = 0;
  int32_t subrsRel = -1;
  while (pos < bytes.size()) {
    const uint8_t b0 = bytes[pos];
    if (b0 >= 32 || b0 == 28 || b0 == 29) {
      ++pos;
      int32_t v = 0;
      if (b0 >= 32 && b0 <= 246) v = static_cast<int32_t>(b0) - 139;
      else if (b0 >= 247 && b0 <= 250) {
        if (pos >= bytes.size()) return false;
        v = (static_cast<int32_t>(b0) - 247) * 256 + bytes[pos++] + 108;
      } else if (b0 >= 251 && b0 <= 254) {
        if (pos >= bytes.size()) return false;
        v = -(static_cast<int32_t>(b0) - 251) * 256 - bytes[pos++] - 108;
      } else if (b0 == 28) {
        if (bytes.size() - pos < 2) return false;
        v = static_cast<int16_t>(rd16(bytes.data() + pos)); pos += 2;
      } else if (b0 == 29) {
        if (bytes.size() - pos < 4) return false;
        v = static_cast<int32_t>(rd32(bytes.data() + pos)); pos += 4;
      } else {
        lastError_ = "unsupported Private DICT number";
        return false;
      }
      stack.push_back(v);
      if (stack.size() > 48) return false;
      continue;
    }

    ++pos;
    uint16_t op = b0;
    if (b0 == 12) {
      if (pos >= bytes.size()) return false;
      op = static_cast<uint16_t>(0x0c00u | bytes[pos++]);
    }
    if (op == 19) {  // Subrs offset, relative to beginning of Private DICT
      if (stack.size() != 1 || stack[0] < 0) {
        lastError_ = "invalid CFF local Subrs offset";
        return false;
      }
      subrsRel = stack[0];
    }
    stack.clear();
  }

  if (subrsRel >= 0) {
    const uint64_t abs = static_cast<uint64_t>(privateDict_.off) + static_cast<uint32_t>(subrsRel);
    if (abs < cff_.off || abs >= static_cast<uint64_t>(cff_.off) + cff_.len) {
      lastError_ = "CFF local Subrs outside table";
      return false;
    }
    if (!parseIndex(static_cast<uint32_t>(abs - cff_.off), localSubrsInfo_, nullptr)) {
      lastError_ = "invalid CFF local Subr INDEX";
      return false;
    }
    localSubrs_ = localSubrsInfo_.whole;
  }
  return true;
}

bool CffFont::executeType2(Slice code, std::vector<Contour>& out, int depth,
                           float& x, float& y, uint32_t& stemCount) const {
  if (depth > 16 || !code.valid() || code.len > 256u * 1024u) {
    lastError_ = "CFF Type2 recursion or CharString limit exceeded";
    return false;
  }

  std::vector<float> stack;
  stack.reserve(48);
  std::function<bool(Slice, int)> run;
  run = [&](Slice slice, int level) -> bool {
    if (level > 16 || !slice.valid() || slice.len > 256u * 1024u) {
      lastError_ = "CFF Type2 subroutine limit exceeded";
      return false;
    }
    std::vector<uint8_t> bytes(slice.len);
    if (!readAt(slice.off, bytes.data(), slice.len)) {
      lastError_ = "failed to read CFF Type2 CharString";
      return false;
    }

    size_t pos = 0;
    while (pos < bytes.size()) {
      const uint8_t b0 = bytes[pos];
      if (b0 == 28 || b0 == 255 || b0 >= 32) {
        float value = 0;
        if (!type2Number(bytes.data(), bytes.size(), pos, value) || stack.size() >= 48) {
          lastError_ = "malformed CFF Type2 operand";
          return false;
        }
        stack.push_back(value);
        continue;
      }

      ++pos;
      uint16_t op = b0;
      if (b0 == 12) {
        if (pos >= bytes.size()) { lastError_ = "truncated CFF Type2 escape"; return false; }
        op = static_cast<uint16_t>(0x0c00u | bytes[pos++]);
      }

      auto clear = [&]() { stack.clear(); };
      auto stem = [&]() {
        const size_t n = stack.size();
        stemCount += static_cast<uint32_t>((n - (n & 1u)) / 2u);
        clear();
      };

      switch (op) {
        case 1: case 3: case 18: case 23:  // stem operators
          stem();
          break;
        case 19: case 20: {  // hintmask / cntrmask
          if (!stack.empty()) stem();
          const size_t maskBytes = (stemCount + 7u) / 8u;
          if (bytes.size() - pos < maskBytes) { lastError_ = "truncated CFF hint mask"; return false; }
          pos += maskBytes;
          break;
        }
        case 4:  // vmoveto
          if (stack.empty()) return false;
          y += stack.back(); moveTo(out, x, y); clear(); break;
        case 21:  // rmoveto
          if (stack.size() < 2) return false;
          x += stack[stack.size()-2]; y += stack.back(); moveTo(out, x, y); clear(); break;
        case 22:  // hmoveto
          if (stack.empty()) return false;
          x += stack.back(); moveTo(out, x, y); clear(); break;
        case 5:  // rlineto
          if (stack.size() < 2 || (stack.size() & 1u)) return false;
          for (size_t i = 0; i < stack.size(); i += 2) { x += stack[i]; y += stack[i+1]; lineTo(out, x, y); }
          clear(); break;
        case 6: case 7: {  // hlineto / vlineto
          bool horiz = (op == 6);
          for (float v : stack) { if (horiz) x += v; else y += v; lineTo(out, x, y); horiz = !horiz; }
          clear(); break;
        }
        case 8:  // rrcurveto
          if (stack.size() < 6 || stack.size() % 6) return false;
          for (size_t i=0;i<stack.size();i+=6) cubicTo(out,x,y,stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]);
          clear(); break;
        case 24: {  // rcurveline
          if (stack.size() < 8 || (stack.size()-2)%6) return false;
          size_t i=0; for (; i+2<stack.size(); i+=6) cubicTo(out,x,y,stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]);
          x += stack[i]; y += stack[i+1]; lineTo(out,x,y); clear(); break;
        }
        case 25: {  // rlinecurve
          if (stack.size() < 8 || (stack.size()-6)%2) return false;
          size_t i=0; for (; i+6<stack.size(); i+=2) { x+=stack[i]; y+=stack[i+1]; lineTo(out,x,y); }
          cubicTo(out,x,y,stack[i],stack[i+1],stack[i+2],stack[i+3],stack[i+4],stack[i+5]); clear(); break;
        }
        case 26: {  // vvcurveto
          size_t i=0; float dx1=0;
          if (stack.size() & 1u) dx1=stack[i++];
          if (stack.size()-i < 4 || (stack.size()-i)%4) return false;
          for (; i<stack.size(); i+=4) { cubicTo(out,x,y,dx1,stack[i],stack[i+1],stack[i+2],0,stack[i+3]); dx1=0; }
          clear(); break;
        }
        case 27: {  // hhcurveto
          size_t i=0; float dy1=0;
          if (stack.size() & 1u) dy1=stack[i++];
          if (stack.size()-i < 4 || (stack.size()-i)%4) return false;
          for (; i<stack.size(); i+=4) { cubicTo(out,x,y,stack[i],dy1,stack[i+1],stack[i+2],stack[i+3],0); dy1=0; }
          clear(); break;
        }
        case 30: case 31: {  // vhcurveto / hvcurveto
          if (stack.size() < 4) return false;
          bool horizontalFirst = (op == 31);
          size_t i=0;
          while (stack.size()-i >= 4) {
            const bool last = (stack.size()-i == 5 || stack.size()-i == 4);
            float a=stack[i], b=stack[i+1], c=stack[i+2], d=stack[i+3], extra=0;
            if (last && stack.size()-i == 5) extra=stack[i+4];
            if (horizontalFirst) cubicTo(out,x,y,a,0,b,c,extra,d);
            else cubicTo(out,x,y,0,a,b,c,d,extra);
            i += (last && stack.size()-i == 5) ? 5 : 4;
            horizontalFirst = !horizontalFirst;
          }
          if (i != stack.size()) return false;
          clear(); break;
        }
        case 10: case 29: {  // callsubr / callgsubr
          if (stack.empty()) return false;
          const int raw = static_cast<int>(std::lround(stack.back())); stack.pop_back();
          const IndexInfo& idx = (op == 10) ? localSubrsInfo_ : globalSubrsInfo_;
          if (idx.count == 0) { lastError_ = "CFF Type2 references missing subroutine INDEX"; return false; }
          const int biased = raw + subrBias(idx.count);
          if (biased < 0 || biased >= idx.count) { lastError_ = "CFF Type2 subroutine index out of range"; return false; }
          Slice sub;
          if (!indexObject(idx, static_cast<uint16_t>(biased), sub) || !run(sub, level+1)) return false;
          break;
        }
        case 11:  // return
          return true;
        case 14:  // endchar
          clear(); return true;
        case 0x0c22: {  // hflex
          if (stack.size()!=7) return false;
          cubicTo(out,x,y,stack[0],0,stack[1],stack[2],stack[3],0);
          cubicTo(out,x,y,stack[4],0,stack[5],-stack[2],stack[6],0); clear(); break;
        }
        case 0x0c23: {  // flex
          if (stack.size()!=13) return false;
          cubicTo(out,x,y,stack[0],stack[1],stack[2],stack[3],stack[4],stack[5]);
          cubicTo(out,x,y,stack[6],stack[7],stack[8],stack[9],stack[10],stack[11]); clear(); break;
        }
        case 0x0c24: {  // hflex1
          if (stack.size()!=9) return false;
          cubicTo(out,x,y,stack[0],stack[1],stack[2],stack[3],stack[4],0);
          cubicTo(out,x,y,stack[5],0,stack[6],stack[7],stack[8],-(stack[1]+stack[3]+stack[7])); clear(); break;
        }
        case 0x0c25: {  // flex1
          if (stack.size()!=11) return false;
          const float dx = stack[0]+stack[2]+stack[4]+stack[6]+stack[8];
          const float dy = stack[1]+stack[3]+stack[5]+stack[7]+stack[9];
          const bool xDominant = std::fabs(dx) > std::fabs(dy);
          cubicTo(out,x,y,stack[0],stack[1],stack[2],stack[3],stack[4],stack[5]);
          cubicTo(out,x,y,stack[6],stack[7],stack[8],stack[9],xDominant?stack[10]:-dx,xDominant?-dy:stack[10]); clear(); break;
        }
        default:
          lastError_ = "unsupported CFF Type2 operator";
          return false;
      }
    }
    return true;
  };

  return run(code, depth);
}

bool CffFont::collectGlyph(uint16_t gid, std::vector<Contour>& out) const {
  if (!ready_ || gid >= glyphCount_) return false;
  Slice glyph;
  if (!indexObject(charStringsInfo_, gid, glyph)) {
    lastError_ = "failed to locate CFF glyph CharString";
    return false;
  }
  float x = 0, y = 0;
  uint32_t stems = 0;
  if (!executeType2(glyph, out, 0, x, y, stems)) return false;
  lastError_ = "ok";
  return true;
}

}  // namespace ttf

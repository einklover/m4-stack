#include "apps/weread/WereadCrypto.h"

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <mbedtls/md5.h>
#define WEREAD_MD5_MBEDTLS 1
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#define WEREAD_MD5_CC 1
#else
#include <openssl/md5.h>
#define WEREAD_MD5_OPENSSL 1
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace weread_crypto {
namespace {

bool isDigitString(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) return false;
  }
  return true;
}

std::string byteHex(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 2);
  char buf[3];
  for (unsigned char c : s) {
    std::snprintf(buf, sizeof(buf), "%x", static_cast<unsigned>(c));
    out += buf;
  }
  return out;
}

// Portable base64 decode (standard alphabet).
bool base64Decode(const std::string& in, std::string& out) {
  static const int8_t T[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
      52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,
      7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
      -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
      49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  out.clear();
  out.reserve(in.size() * 3 / 4 + 4);
  int val = 0, valb = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    int d = T[c];
    if (d == -1) continue;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return !out.empty();
}

std::string checkedBody(const std::string& shard) {
  if (shard.size() <= 32) return {};
  const std::string expected = shard.substr(0, 32);
  const std::string body = shard.substr(32);
  std::string actual = md5Hex(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  for (char& c : actual) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (actual != expected) return {};
  return body;
}

std::vector<int> swapPositions(const char* enc, int length) {
  std::vector<int> result;
  if (length < 4) return result;
  if (length < 11) {
    result.push_back(0);
    result.push_back(2);
    return result;
  }
  const int n = std::min(4, (length + 9) / 10);
  std::string tmp;
  for (int i = length - 1; i >= length - n; i--) {
    uint8_t v = static_cast<uint8_t>(enc[i]);
    uint32_t val = 0;
    for (int b = 0; b < 8; b++) {
      if ((v >> b) & 1) val += (1U << (2 * b));
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", val);
    tmp += buf;
  }
  const int m = length - n - 2;
  if (m <= 0) return result;
  const int step = static_cast<int>(std::to_string(m).size());
  int i = 0;
  while (static_cast<int>(result.size()) < 10 && i + step < static_cast<int>(tmp.size())) {
    int v1 = 0, v2 = 0;
    try {
      v1 = std::stoi(tmp.substr(static_cast<size_t>(i), static_cast<size_t>(step))) % m;
      v2 = std::stoi(tmp.substr(static_cast<size_t>(i + 1), static_cast<size_t>(step))) % m;
    } catch (...) {
      break;
    }
    result.push_back(v1);
    result.push_back(v2);
    i += step;
  }
  return result;
}

void reverseSwapsInPlace(char* enc, int length, const std::vector<int>& pos) {
  const int plen = static_cast<int>(pos.size());
  for (int i = plen - 1; i > 0; i -= 2) {
    for (int k = 1; k >= 0; k--) {
      const int left = pos[static_cast<size_t>(i)] + k;
      const int right = pos[static_cast<size_t>(i - 1)] + k;
      if (left >= 0 && right >= 0 && left < length && right < length) {
        const char t = enc[left];
        enc[left] = enc[right];
        enc[right] = t;
      }
    }
  }
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

}  // namespace

std::string md5Hex(const uint8_t* data, size_t len) {
  uint8_t out[16];
#if defined(WEREAD_MD5_MBEDTLS)
  mbedtls_md5(data, len, out);
#elif defined(WEREAD_MD5_CC)
  CC_MD5(data, static_cast<CC_LONG>(len), out);
#else
  MD5(data, len, out);
#endif
  char hex[33];
  for (int i = 0; i < 16; i++) {
    std::snprintf(hex + i * 2, 3, "%02x", out[i]);
  }
  hex[32] = 0;
  return std::string(hex);
}

std::string md5Hex(const std::string& s) {
  return md5Hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  char buf[4];
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string e(const std::string& value) {
  const std::string h = md5Hex(value);
  std::string result = h.substr(0, 3);
  // Wire format (must match Lua/Python probes and papers3-weread):
  //   md5[:3] + typeFlag + "2" + md5[-2:] + chunks...
  // A prior rewrite appended md5[-2:] *after* the chunks, which made every
  // reader URL and chapter signature wrong and WeRead returned HTTP 404
  // ("章节不存在"). Keep chunk assembly allocation-light, but never reorder.
  result.reserve(value.size() * 4u + 64u);
  bool firstChunk = true;
  const auto appendChunk = [&](const char* data, size_t len) {
    if (!firstChunk) result += 'g';
    firstChunk = false;
    char lenbuf[4];
    std::snprintf(lenbuf, sizeof(lenbuf), "%02x", static_cast<unsigned>(len));
    result += lenbuf;
    result.append(data, len);
  };

  if (isDigitString(value)) {
    result += "32";
    if (h.size() >= 2) result += h.substr(h.size() - 2);
    size_t i = 0;
    while (i < value.size()) {
      const size_t end = std::min(i + 9, value.size());
      const std::string part = value.substr(i, end - i);
      unsigned long long v = std::strtoull(part.c_str(), nullptr, 10);
      char buf[24];
      std::snprintf(buf, sizeof(buf), "%llx", v);
      appendChunk(buf, std::strlen(buf));
      i = end;
    }
  } else {
    result += "42";
    if (h.size() >= 2) result += h.substr(h.size() - 2);
    const std::string chunk = byteHex(value);
    appendChunk(chunk.data(), chunk.size());
  }

  while (result.size() < 20) {
    result += h.substr(0, 20 - result.size());
  }

  result += md5Hex(result).substr(0, 3);
  return result;
}

std::string sign(const std::string& query) {
  int64_t a = 0x15051505;
  int64_t b = a;
  const int length = static_cast<int>(query.size());
  int i = length;

  while (i > 1) {
    const unsigned char ci = static_cast<unsigned char>(query[static_cast<size_t>(i - 1)]);
    const unsigned char ci1 = static_cast<unsigned char>(query[static_cast<size_t>(i - 2)]);
    a = (a ^ (static_cast<int64_t>(ci) << ((length - i + 1) % 30))) & 0x7fffffff;
    b = (b ^ (static_cast<int64_t>(ci1) << ((i - 1) % 30))) & 0x7fffffff;
    i -= 2;
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(a + b));
  std::string s(buf);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string sortedQueryWithSign(const std::vector<std::pair<std::string, std::string>>& params,
                                std::string& outQueryForSign) {
  std::vector<std::pair<std::string, std::string>> sorted = params;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  outQueryForSign.clear();
  std::string full;
  for (const auto& kv : sorted) {
    if (kv.first == "s") continue;
    if (!outQueryForSign.empty()) outQueryForSign += '&';
    outQueryForSign += kv.first;
    outQueryForSign += '=';
    outQueryForSign += urlEncode(kv.second);
  }
  const std::string s = sign(outQueryForSign);
  full = outQueryForSign;
  if (!full.empty()) full += '&';
  full += "s=";
  full += s;
  return full;
}

std::string makeContentParamsJson(const std::string& bookId, const std::string& chapterUid,
                                  const std::string& psvts, bool style, int sc, long unixTimeSec,
                                  long rnd0to9999) {
  long ct = unixTimeSec;
  if (e(std::to_string(ct)) == psvts) ct += 1;
  if (rnd0to9999 < 0) rnd0to9999 = 0;
  if (rnd0to9999 > 9999) rnd0to9999 = 9999;
  const long r = rnd0to9999 * rnd0to9999;

  std::vector<std::pair<std::string, std::string>> p;
  p.push_back({"b", e(bookId)});
  p.push_back({"c", e(chapterUid)});
  p.push_back({"r", std::to_string(r)});
  p.push_back({"ct", std::to_string(ct)});
  p.push_back({"ps", psvts});
  p.push_back({"pc", e(std::to_string(ct))});
  p.push_back({"sc", std::to_string(sc)});
  p.push_back({"prevChapter", "false"});
  p.push_back({"st", style ? "1" : "0"});

  std::string forSign;
  sortedQueryWithSign(p, forSign);
  // re-sign into params
  const std::string s = sign(forSign);

  // JSON: numeric fields unquoted: ct, sc, st, r
  auto isNum = [](const std::string& k) {
    return k == "ct" || k == "sc" || k == "st" || k == "r";
  };
  p.push_back({"s", s});
  std::string j = "{";
  bool first = true;
  for (const auto& kv : p) {
    if (!first) j += ',';
    first = false;
    j += '"';
    j += kv.first;
    j += "\":";
    if (isNum(kv.first)) {
      j += kv.second;
    } else {
      j += '"';
      j += jsonEscape(kv.second);
      j += '"';
    }
  }
  j += '}';
  return j;
}

std::string decodeContentShards(const std::string& s0, const std::string& s1, const std::string& s2) {
  std::string payload = checkedBody(s0) + checkedBody(s1) + checkedBody(s2);
  if (payload.empty()) return {};
  if (payload.size() < 2) return {};

  // Drop first char, reverse swaps, base64url decode
  std::string enc = payload.substr(1);
  std::vector<int> pos = swapPositions(enc.data(), static_cast<int>(enc.size()));
  reverseSwapsInPlace(&enc[0], static_cast<int>(enc.size()), pos);

  std::string b64;
  b64.reserve(enc.size() + 4);
  for (char c : enc) {
    if (c == '-')
      b64 += '+';
    else if (c == '_')
      b64 += '/';
    else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')
      b64 += c;
  }
  while (b64.size() % 4) b64 += '=';

  std::string plain;
  if (!base64Decode(b64, plain)) return {};
  return plain;
}

std::string extractPsvts(const std::string& html) {
  const auto idx = html.find("\"psvts\"");
  if (idx == std::string::npos) return {};
  const auto colon = html.find(':', idx);
  if (colon == std::string::npos) return {};
  const auto q1 = html.find('"', colon + 1);
  if (q1 == std::string::npos) return {};
  const auto q2 = html.find('"', q1 + 1);
  if (q2 == std::string::npos) return {};
  return html.substr(q1 + 1, q2 - q1 - 1);
}

std::string stripXhtml(const std::string& xhtml) {
  std::string out;
  out.reserve(xhtml.size());
  bool inTag = false;
  bool inEntity = false;
  std::string ent;
  for (size_t i = 0; i < xhtml.size(); i++) {
    char c = xhtml[i];
    if (inTag) {
      if (c == '>') inTag = false;
      continue;
    }
    if (c == '<') {
      // skip <style>...</style> and <script>
      if (i + 6 < xhtml.size()) {
        std::string head = xhtml.substr(i, 6);
        for (char& ch : head) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (head == "<style") {
          auto end = xhtml.find("</style>", i);
          if (end == std::string::npos) end = xhtml.find("</STYLE>", i);
          if (end != std::string::npos) {
            i = end + 7;
            continue;
          }
        }
      }
      inTag = true;
      continue;
    }
    if (c == '&') {
      inEntity = true;
      ent.clear();
      continue;
    }
    if (inEntity) {
      if (c == ';') {
        if (ent == "nbsp" || ent == "#160")
          out += ' ';
        else if (ent == "lt")
          out += '<';
        else if (ent == "gt")
          out += '>';
        else if (ent == "amp")
          out += '&';
        else if (ent == "quot")
          out += '"';
        else if (ent == "apos")
          out += '\'';
        // else drop
        inEntity = false;
      } else {
        ent += c;
        if (ent.size() > 10) {
          out += '&';
          out += ent;
          inEntity = false;
        }
      }
      continue;
    }
    if (c == '\r') continue;
    out += c;
  }
  // collapse excessive blank lines
  std::string norm;
  int nl = 0;
  for (char c : out) {
    if (c == '\n') {
      if (++nl <= 2) norm += c;
    } else {
      nl = 0;
      norm += c;
    }
  }
  return norm;
}

}  // namespace weread_crypto

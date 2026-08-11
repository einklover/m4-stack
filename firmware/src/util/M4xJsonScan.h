#pragma once

// Lightweight JSON array extractor (host-testable, no ArduinoJson).
// Scans a JSON document for `marker`-style paths, walks nested arrays and
// emits each object's fields as one tab-separated line. Unlike ArduinoJson it
// never materializes the whole document, so multi-hundred-KB JSON (fanqie TOCs)
// cannot exhaust the device's internal RAM. Used by M4xLuaHost dl.jsonToFile.

#include <cstddef>
#include <string>
#include <vector>

namespace M4xJsonScan {

// Find `"name"` followed by ':' (JSON object field) at or after `pos`.
inline size_t findField(const std::string& s, size_t pos, const std::string& name) {
  const std::string pat = "\"" + name + "\"";
  size_t p = pos;
  while ((p = s.find(pat, p)) != std::string::npos) {
    size_t q = p + pat.size();
    while (q < s.size() && (s[q] == ' ' || s[q] == '\t')) ++q;
    if (q < s.size() && s[q] == ':') return p;
    p = q;
  }
  return std::string::npos;
}

// Decode a JSON string starting at s[startIdx] == '"'. Sets endIdx past the
// closing quote. Handles \\, \", \/, \n, \t, \r, \b, \f and \uXXXX (best-effort
// surrogate pairs → UTF-8). Returns false on unterminated/ill-formed input.
inline bool readString(const std::string& s, size_t startIdx, std::string& out, size_t& endIdx) {
  out.clear();
  if (startIdx >= s.size() || s[startIdx] != '"') return false;
  size_t i = startIdx + 1;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') {
      endIdx = i + 1;
      return true;
    }
    if (c == '\\') {
      if (i + 1 >= s.size()) return false;
      const char e = s[i + 1];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (i + 6 > s.size()) return false;
          unsigned int code = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = s[i + 2 + static_cast<size_t>(k)];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
          }
          if (code >= 0xD800 && code <= 0xDBFF && i + 12 <= s.size() && s[i + 6] == '\\' &&
              s[i + 7] == 'u') {
            unsigned int lo = 0;
            for (int k = 0; k < 4; ++k) {
              const char h = s[i + 8 + static_cast<size_t>(k)];
              lo <<= 4;
              if (h >= '0' && h <= '9') lo |= static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned>(h - 'A' + 10);
              else return false;
            }
            i += 6;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              code = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              code = 0xFFFD;
            }
          }
          if (code < 0x80) {
            out += static_cast<char>(code);
          } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          }
          // The common escape advance below consumes "\\u" (2 bytes); skip
          // the four hex digits here. A surrogate pair already added 6 for
          // its second "\\uXXXX", so the total advance is 12 bytes.
          i += 4;
          break;
        }
        default:
          return false;
      }
      i += 2;
    } else {
      out += c;
      ++i;
    }
  }
  return false;
}

// Extract records from a nested JSON array starting at s[arrStart]=='['.
// Recurses into nested arrays (flatten). Each record = fields joined by '\t'
// plus '\n', appended to `out`. count/maxRecords bound the result.
inline bool extractRecords(const std::string& s, size_t arrStart, const std::vector<std::string>& fields,
                           std::string& out, size_t& count, size_t maxRecords) {
  size_t i = arrStart + 1;
  while (i < s.size() && count < maxRecords) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == ',')) ++i;
    if (i >= s.size()) return true;
    const char c = s[i];
    if (c == ']') return true;
    if (c == '[') {
      if (!extractRecords(s, i, fields, out, count, maxRecords)) return false;
      int depth = 0;
      while (i < s.size()) {
        if (s[i] == '[') ++depth;
        else if (s[i] == ']') { --depth; if (depth == 0) { ++i; break; } }
        ++i;
      }
      continue;
    }
    if (c == '{') {
      std::string rec;
      bool haveAny = false;
      for (const auto& f : fields) {
        const size_t fp = findField(s, i + 1, f);
        if (fp == std::string::npos) {
          rec += "";
        } else {
          size_t colon = fp + f.size() + 2;  // skip "\"name\""
          if (colon < s.size() && s[colon] == ':') ++colon;
          while (colon < s.size() && (s[colon] == ' ' || s[colon] == '\t')) ++colon;
          std::string val;
          size_t end;
          if (colon < s.size() && s[colon] == '"' && readString(s, colon, val, end)) {
            rec += val;
            haveAny = true;
          } else {
            rec += "";
          }
        }
        rec += '\t';
      }
      if (haveAny) {
        if (!rec.empty()) rec.pop_back();
        rec += '\n';
        out += rec;
        ++count;
      }
      int depth = 0;
      while (i < s.size()) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}') { --depth; if (depth == 0) { ++i; break; } }
        ++i;
      }
      continue;
    }
    ++i;  // skip primitive
  }
  return true;
}

}  // namespace M4xJsonScan

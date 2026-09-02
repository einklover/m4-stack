#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace M4HttpRequestParser {

using Field = std::pair<std::string, std::string>;

inline int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::string urlDecode(const std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '+') {
      out.push_back(' ');
      continue;
    }
    if (c == '%' && i + 2 < value.size()) {
      const int hi = hexValue(value[i + 1]);
      const int lo = hexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

inline bool extractMultipartBoundary(const std::string_view contentType, std::string& boundary) {
  boundary.clear();
  constexpr std::string_view key = "boundary=";
  const size_t keyPos = contentType.find(key);
  if (keyPos == std::string_view::npos) return false;

  size_t start = keyPos + key.size();
  while (start < contentType.size() && std::isspace(static_cast<unsigned char>(contentType[start]))) ++start;
  if (start >= contentType.size()) return false;

  if (contentType[start] == '"') {
    ++start;
    const size_t end = contentType.find('"', start);
    if (end == std::string_view::npos || end == start) return false;
    boundary.assign(contentType.substr(start, end - start));
    return true;
  }

  size_t end = contentType.find(';', start);
  if (end == std::string_view::npos) end = contentType.size();
  while (end > start && std::isspace(static_cast<unsigned char>(contentType[end - 1]))) --end;
  if (end == start) return false;
  boundary.assign(contentType.substr(start, end - start));
  return true;
}

inline bool extractDispositionParameter(const std::string_view headers, const std::string_view key,
                                        std::string& value) {
  value.clear();
  std::string pattern;
  pattern.reserve(key.size() + 2);
  pattern.append(key);
  pattern.append("=\"");
  const size_t startPos = headers.find(pattern);
  if (startPos == std::string_view::npos) return false;
  const size_t start = startPos + pattern.size();
  const size_t end = headers.find('"', start);
  if (end == std::string_view::npos) return false;
  value.assign(headers.substr(start, end - start));
  return true;
}

inline bool extractMultipartFilename(const std::string_view headers, std::string& filename) {
  return extractDispositionParameter(headers, "filename", filename) && !filename.empty();
}

inline std::string fieldValue(const std::vector<Field>& fields, const std::string_view key) {
  for (const auto& field : fields) {
    if (field.first == key) return field.second;
  }
  return {};
}

inline bool parseUrlEncodedFields(const std::string_view body, std::vector<Field>& fields) {
  fields.clear();
  size_t offset = 0;
  while (offset <= body.size()) {
    size_t end = body.find('&', offset);
    if (end == std::string_view::npos) end = body.size();
    const std::string_view item = body.substr(offset, end - offset);
    if (!item.empty()) {
      const size_t equals = item.find('=');
      const std::string_view rawKey = item.substr(0, equals);
      const std::string_view rawValue = equals == std::string_view::npos ? std::string_view{} : item.substr(equals + 1);
      fields.emplace_back(urlDecode(rawKey), urlDecode(rawValue));
    }
    if (end == body.size()) break;
    offset = end + 1;
  }
  return true;
}

inline bool parseMultipartFields(const std::string_view body, const std::string_view boundary,
                                 std::vector<Field>& fields) {
  fields.clear();
  if (boundary.empty()) return false;

  const std::string marker = "--" + std::string(boundary);
  const std::string nextMarker = "\r\n" + marker;
  size_t cursor = 0;

  while (true) {
    const size_t markerPos = body.find(marker, cursor);
    if (markerPos == std::string_view::npos) return !fields.empty();
    size_t partStart = markerPos + marker.size();
    if (partStart + 2 <= body.size() && body.substr(partStart, 2) == "--") return true;
    if (partStart + 2 > body.size() || body.substr(partStart, 2) != "\r\n") return false;
    partStart += 2;

    const size_t headersEnd = body.find("\r\n\r\n", partStart);
    if (headersEnd == std::string_view::npos) return false;
    const std::string_view headers = body.substr(partStart, headersEnd - partStart);

    std::string name;
    if (!extractDispositionParameter(headers, "name", name) || name.empty()) return false;

    const size_t valueStart = headersEnd + 4;
    const size_t next = body.find(nextMarker, valueStart);
    if (next == std::string_view::npos) return false;
    fields.emplace_back(std::move(name), std::string(body.substr(valueStart, next - valueStart)));
    cursor = next + 2;
  }
}

inline size_t findTerminalBoundary(const std::string_view tail, const std::string_view boundary) {
  if (boundary.empty()) return std::string::npos;
  const std::string marker = "\r\n--" + std::string(boundary) + "--";
  const size_t pos = tail.rfind(marker);
  if (pos == std::string_view::npos) return std::string::npos;
  const size_t after = pos + marker.size();
  if (after == tail.size()) return pos;
  if (after + 2 == tail.size() && tail.substr(after, 2) == "\r\n") return pos;
  return std::string::npos;
}

}  // namespace M4HttpRequestParser

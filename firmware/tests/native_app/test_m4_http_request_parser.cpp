#include <cassert>
#include <string>
#include <utility>
#include <vector>

#include "../../src/network/M4HttpRequestParser.h"

int main() {
  using M4HttpRequestParser::Field;

  assert(M4HttpRequestParser::urlDecode("%2FBooks%2F%E4%B8%AD%E6%96%87+EPUB") == "/Books/中文 EPUB");
  assert(M4HttpRequestParser::urlDecode("plain-value") == "plain-value");

  std::string boundary;
  assert(M4HttpRequestParser::extractMultipartBoundary(
      "multipart/form-data; boundary=----WebKitFormBoundaryABC123", boundary));
  assert(boundary == "----WebKitFormBoundaryABC123");
  assert(M4HttpRequestParser::extractMultipartBoundary(
      "multipart/form-data; boundary=\"quoted-boundary\"", boundary));
  assert(boundary == "quoted-boundary");
  assert(!M4HttpRequestParser::extractMultipartBoundary("application/json", boundary));

  const std::string formBoundary = "----Boundary42";
  const std::string multipart =
      "------Boundary42\r\n"
      "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
      "%2FBooks%2FSci-Fi\r\n"
      "------Boundary42\r\n"
      "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
      "Dune.epub\r\n"
      "------Boundary42--\r\n";
  std::vector<Field> fields;
  assert(M4HttpRequestParser::parseMultipartFields(multipart, formBoundary, fields));
  assert(M4HttpRequestParser::fieldValue(fields, "path") == "%2FBooks%2FSci-Fi");
  assert(M4HttpRequestParser::fieldValue(fields, "name") == "Dune.epub");

  fields.clear();
  assert(M4HttpRequestParser::parseUrlEncodedFields("path=%2FBooks%2FNew+Folder&name=Hello%20World", fields));
  assert(M4HttpRequestParser::fieldValue(fields, "path") == "/Books/New Folder");
  assert(M4HttpRequestParser::fieldValue(fields, "name") == "Hello World");

  const std::string uploadHeaders =
      "------Boundary42\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"book.epub\"\r\n"
      "Content-Type: application/epub+zip\r\n\r\n";
  std::string filename;
  assert(M4HttpRequestParser::extractMultipartFilename(uploadHeaders, filename));
  assert(filename == "book.epub");

  const std::string uploadTail = std::string("binary-data\0still-binary", 24) +
                                 "\r\n------Boundary42--\r\n";
  const size_t terminal = M4HttpRequestParser::findTerminalBoundary(uploadTail, formBoundary);
  assert(terminal == 24);
  assert(M4HttpRequestParser::findTerminalBoundary("no boundary here", formBoundary) == std::string::npos);

  return 0;
}

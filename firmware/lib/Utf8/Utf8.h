#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);
// Keep the first maxChars UTF-8 codepoints. If the string is longer, append
// ellipsis (default U+2026). Empty / null input yields an empty string.
std::string utf8EllipsizeChars(const char* text, size_t maxChars,
                               const char* ellipsis = "…");

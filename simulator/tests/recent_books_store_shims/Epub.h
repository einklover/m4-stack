#pragma once

#include <string>

class Epub {
 public:
  Epub(const std::string&, const std::string&) {}
  void load(bool) {}
  std::string getTitle() const { return {}; }
  std::string getAuthor() const { return {}; }
  std::string getThumbBmpPath() const { return {}; }
};

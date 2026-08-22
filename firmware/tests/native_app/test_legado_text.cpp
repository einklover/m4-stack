// Host regression for streaming Legado/XHTML chapter-body cleanup.
#include "apps/providers/M4NativeProviderText.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

class StringSink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    if (!data && len != 0) return false;
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  }

  std::string body;
};

}  // namespace

int main() {
  StringSink out;
  M4NativeProviderText::XhtmlStripSink strip(out);
  const std::string input =
      "<img src=\"cover.jpg\">\r\n2008 &amp; beyond<br/>第二<br />段</p>"
      "<div>尾</div>&lt;ok&gt;";

  // Feed one byte at a time so tags/entities crossing HTTP chunks stay covered.
  for (unsigned char c : input) assert(strip.write(&c, 1));
  assert(out.body == "\n2008 & beyond\n第二\n段\n尾\n<ok>");
  assert(out.body.find("<img") == std::string::npos);
  std::printf("legado streaming XHTML cleanup: PASS\n");
  return 0;
}

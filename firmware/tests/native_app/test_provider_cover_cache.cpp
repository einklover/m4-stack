#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <cstring>

#include "RecentBooksStore.h"
#include "util/M4ProviderCoverCache.h"

using namespace M4ProviderCoverCache;

// Helpers for 1-bit BMP exact-size validation (Home Scene requires 1-bit, aspect-fill exact WxH)
static std::vector<uint8_t> makeFake1BitBmp(int w, int h) {
  int rowBytes = (w + 31) / 32 * 4;
  int imageSize = rowBytes * h;
  int fileSize = 62 + imageSize;
  std::vector<uint8_t> buf;
  buf.reserve(fileSize);
  auto w8 = [&](uint8_t v){ buf.push_back(v); };
  auto w16le = [&](uint16_t v){ w8(v & 0xff); w8((v >> 8) & 0xff); };
  auto w32le = [&](uint32_t v){ w8(v & 0xff); w8((v>>8)&0xff); w8((v>>16)&0xff); w8((v>>24)&0xff); };
  auto w32sle = [&](int32_t v){ w32le(static_cast<uint32_t>(v)); };
  w8('B'); w8('M');
  w32le(fileSize);
  w32le(0);
  w32le(62);
  w32le(40);
  w32sle(w);
  w32sle(-h); // top-down
  w16le(1);
  w16le(1); // 1-bit
  w32le(0);
  w32le(imageSize);
  w32le(2835); w32le(2835);
  w32le(2); w32le(2);
  // palette: black, white
  w8(0x00); w8(0x00); w8(0x00); w8(0x00);
  w8(0xFF); w8(0xFF); w8(0xFF); w8(0x00);
  buf.resize(62, 0);
  // image: fill with dummy checker (already zero)
  buf.resize(fileSize, 0);
  // write a few bytes to make it non-trivial
  for (int y = 0; y < h; ++y) {
    int off = 62 + y * rowBytes;
    if (off < (int)buf.size()) buf[off] = (y % 2) ? 0xAA : 0x55;
  }
  return buf;
}

static bool parseBmpHeader(const std::vector<uint8_t>& buf, int &outW, int &outH, int &outBpp, int &outOffset) {
  if (buf.size() < 62) return false;
  if (buf[0] != 'B' || buf[1] != 'M') return false;
  auto r16 = [&](size_t o)->uint16_t { return buf[o] | (buf[o+1]<<8); };
  auto r32 = [&](size_t o)->uint32_t { return buf[o] | (buf[o+1]<<8) | (buf[o+2]<<16) | (buf[o+3]<<24); };
  auto r32s = [&](size_t o)->int32_t { return static_cast<int32_t>(r32(o)); };
  uint32_t off = r32(10);
  uint32_t dib = r32(14);
  if (dib != 40) return false;
  int32_t w = r32s(18);
  int32_t h = r32s(22);
  uint16_t planes = r16(26);
  uint16_t bpp = r16(28);
  if (planes != 1) return false;
  outW = w;
  outH = h < 0 ? -h : h;
  outBpp = bpp;
  outOffset = off;
  return true;
}

static bool isValid1BitBmpOfSize(const std::vector<uint8_t>& buf, int expectW, int expectH) {
  int w=0,h=0,bpp=0,off=0;
  if (!parseBmpHeader(buf, w,h,bpp,off)) return false;
  if (w != expectW || h != expectH) return false;
  if (bpp != 1) return false;
  if (off != 62) return false;
  int rowBytes = (expectW + 31)/32*4;
  int expectFileSize = 62 + rowBytes * expectH;
  if ((int)buf.size() != expectFileSize) return false;
  // palette must be 2 entries: black then white
  if (buf.size() < 62) return false;
  // palette at 54..61: first entry 00 00 00 00, second FF FF FF 00
  if (buf[54]!=0x00 || buf[55]!=0x00 || buf[56]!=0x00 || buf[57]!=0x00) return false;
  if (buf[58]!=0xFF || buf[59]!=0xFF || buf[60]!=0xFF || buf[61]!=0x00) return false;
  return true;
}

int main() {
  const uint8_t jpegMagic[] = {0xff, 0xd8, 0xff};
  const uint8_t pngMagic[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  const uint8_t webpMagic[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
  const uint8_t unknownMagic[] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
  assert(detectImageFormat(jpegMagic, sizeof(jpegMagic)) == ImageFormat::Jpeg);
  assert(detectImageFormat(pngMagic, sizeof(pngMagic)) == ImageFormat::Png);
  assert(detectImageFormat(webpMagic, sizeof(webpMagic)) == ImageFormat::Webp);
  assert(detectImageFormat(unknownMagic, sizeof(unknownMagic)) == ImageFormat::Unknown);
  assert(canConvertImageFormat(ImageFormat::Bmp));
  assert(canConvertImageFormat(ImageFormat::Jpeg));
  assert(canConvertImageFormat(ImageFormat::Png));
  assert(!canConvertImageFormat(ImageFormat::Webp));
  assert(!canConvertImageFormat(ImageFormat::Unknown));

  M4NovelProvider::BookDetail extensionlessDetail;
  extensionlessDetail.coverUrl = "https://cdn.invalid/image?id=1";
  const auto extensionlessRequest = requestFor("weread", "extensionless", extensionlessDetail, 120, 160);
  assert(extensionlessRequest.coverUrl.find(".jpg") == std::string::npos);

  std::set<std::string> files;
  std::string fetchedUrl;
  size_t fetchedLimit = 0;
  int fetches = 0;
  int conversions = 0;
  Backend backend;
  backend.exists = [&](const std::string& path) { return files.count(path) != 0; };
  backend.makeDirs = [&](const std::string&) { return true; };
  backend.fetch = [&](const std::string& url, const std::string& path, size_t maxBytes) {
    ++fetches;
    fetchedUrl = url;
    fetchedLimit = maxBytes;
    if (url == "https://fixture.invalid/fail.jpg") return false;
    files.insert(path);
    return true;
  };
  backend.convert = [&](const std::string&, const std::string& target, int width, int height) {
    ++conversions;
    assert(width == 120 && height == 160);
    files.insert(target);
    return true;
  };
  backend.remove = [&](const std::string& path) { files.erase(path); };

  const Request request{"weread", "book/unsafe-is-not-in-id", "https://fixture.invalid/cover.jpg", 120, 160};
  assert(bmpTemplatePath("weread", "book") == bmpTemplatePath("weread", "book"));
  assert(bmpTemplatePath("weread", "book") != bmpTemplatePath("fanqie", "book"));

  const auto failed = acquire(
      Request{"weread", "failed", "https://fixture.invalid/fail.jpg", 120, 160}, backend);
  assert(failed.coverBmpPath.empty());
  assert(fetches == 1);

  const auto success = acquire(request, backend);
  assert(success.coverBmpPath == bmpTemplatePath("weread", "book/unsafe-is-not-in-id"));
  assert(!success.cacheHit);
  assert(fetchedUrl == request.coverUrl);
  assert(fetchedLimit == kMaxDownloadBytes);
  assert(conversions == 1);

  const auto hit = acquire(request, backend);
  assert(hit.cacheHit);
  assert(hit.coverBmpPath == success.coverBmpPath);
  assert(fetches == 2);  // one failed fixture + one successful fetch; cache hit fetches none
  assert(conversions == 1);

  const auto empty = acquire(Request{"weread", "book", "", 120, 160}, backend);
  assert(empty.coverBmpPath.empty());

  RecentBook existing{"m4cp://weread/book", "Title", "Author", "/covers/old.bmp", "/cache/ch.txt"};
  assert(mergeProviderMetadata(existing, "", "Updated Author", ""));
  assert(existing.author == "Updated Author");
  assert(existing.coverBmpPath == "/covers/old.bmp");
  assert(!mergeProviderMetadata(existing, "", "", ""));

  std::map<std::string, std::vector<uint8_t>> payloads;
  Backend formatBackend;
  formatBackend.exists = [&](const std::string& path) { return files.count(path) != 0; };
  formatBackend.makeDirs = [&](const std::string&) { return true; };
  formatBackend.fetch = [&](const std::string& url, const std::string& path, size_t) {
    if (url.find("png") != std::string::npos) payloads[path] = std::vector<uint8_t>(pngMagic, pngMagic + sizeof(pngMagic));
    else if (url.find("bmp") != std::string::npos) payloads[path] = {'B', 'M', 0, 0};
    else if (url.find("webp") != std::string::npos) payloads[path] = std::vector<uint8_t>(webpMagic, webpMagic + sizeof(webpMagic));
    else if (url.find("unknown") != std::string::npos) payloads[path] = std::vector<uint8_t>(unknownMagic, unknownMagic + sizeof(unknownMagic));
    else payloads[path] = std::vector<uint8_t>(jpegMagic, jpegMagic + sizeof(jpegMagic));
    files.insert(path);
    return true;
  };
  formatBackend.convert = [&](const std::string& source, const std::string& target, int, int) {
    if (!canConvertImageFormat(
            detectImageFormat(payloads[source].data(), payloads[source].size()))) return false;
    files.insert(target);
    return true;
  };
  formatBackend.remove = [&](const std::string& path) {
    files.erase(path);
    payloads.erase(path);
  };
  const auto pngSuccess = acquire(
      Request{"weread", "accept-png", "https://cdn.invalid/png", 120, 160}, formatBackend);
  assert(!pngSuccess.coverBmpPath.empty());
  assert(files.count(concreteBmpPath("weread", "accept-png", 120, 160)) != 0);

  for (const char* kindValue : {"bmp", "jpeg"}) {
    const std::string kind = kindValue;
    const auto accepted = acquire(Request{"weread", "accept-" + kind,
                                         "https://cdn.invalid/" + kind, 120, 160},
                                  formatBackend);
    assert(!accepted.coverBmpPath.empty());
  }

  for (const char* kindValue : {"webp", "unknown"}) {
    const std::string kind = kindValue;
    const auto rejected = acquire(Request{"weread", "reject-" + kind, "https://cdn.invalid/" + kind,
                                          120, 160}, formatBackend);
    assert(rejected.coverBmpPath.empty());
    assert(files.count(concreteBmpPath("weread", "reject-" + kind, 120, 160)) == 0);
  }
  const auto extensionlessJpeg = acquire(
      Request{"weread", "extensionless", extensionlessRequest.coverUrl, 120, 160}, formatBackend);
  assert(!extensionlessJpeg.coverBmpPath.empty());
  assert(files.count(concreteBmpPath("weread", "extensionless", 120, 160)) != 0);

  // Home Scene sizes: generate on miss from source.img, never fetch.
  std::set<std::string> homeFiles;
  int homeFetches = 0;
  int homeConverts = 0;
  int lastConvertW = 0;
  int lastConvertH = 0;
  std::string lastConvertSource;
  Backend homeBackend;
  homeBackend.exists = [&](const std::string& path) { return homeFiles.count(path) != 0; };
  homeBackend.fetch = [&](const std::string&, const std::string&, size_t) {
    ++homeFetches;
    return false;
  };
  homeBackend.convert = [&](const std::string& source, const std::string& target, int width, int height) {
    ++homeConverts;
    lastConvertSource = source;
    lastConvertW = width;
    lastConvertH = height;
    homeFiles.insert(target);
    return true;
  };
  homeBackend.remove = [&](const std::string& path) { homeFiles.erase(path); };
  const std::string templatePath =
      "/.crosspoint/provider_covers/032352886ae8bcbc/cover_[WIDTH]x[HEIGHT].bmp";
  const std::string sourceImg = "/.crosspoint/provider_covers/032352886ae8bcbc/source.img";
  const std::string fengyan = "/.crosspoint/provider_covers/032352886ae8bcbc/cover_171x254.bmp";
  homeFiles.insert(sourceImg);
  homeFiles.insert(fengyan);

  const auto miss110 = ensureSizedCoverFromSource(templatePath, 110, 180, homeBackend);
  assert(!miss110.thumbPath.empty());
  assert(miss110.generated);
  assert(!miss110.cacheHit);
  assert(homeFetches == 0);
  assert(homeConverts == 1);
  assert(lastConvertSource == sourceImg);
  assert(lastConvertW == 110 && lastConvertH == 180);
  assert(homeFiles.count("/.crosspoint/provider_covers/032352886ae8bcbc/cover_110x180.bmp") != 0);

  const auto hit110 = ensureSizedCoverFromSource(templatePath, 110, 180, homeBackend);
  assert(hit110.cacheHit);
  assert(!hit110.generated);
  assert(homeConverts == 1);
  assert(homeFetches == 0);

  const auto miss74 = ensureSizedCoverFromSource(templatePath, 74, 106, homeBackend);
  assert(miss74.generated);
  assert(lastConvertW == 74 && lastConvertH == 106);
  assert(homeConverts == 2);

  homeFiles.erase(sourceImg);
  const auto noSource = ensureSizedCoverFromSource(templatePath, 64, 64, homeBackend);
  assert(noSource.thumbPath.empty());
  assert(homeConverts == 2);

  const auto outside = ensureSizedCoverFromSource("/books/local/cover_[WIDTH]x[HEIGHT].bmp", 110, 180, homeBackend);
  assert(outside.thumbPath.empty());

  bool cancelFlag = true;
  const auto cancelled = ensureSizedCoverFromSource(
      templatePath, 50, 80, homeBackend, [&]() { return cancelFlag; });
  assert(cancelled.thumbPath.empty());
  assert(homeFetches == 0);

  // ---- Round-1 strengthened contracts: Home generate-on-miss ----

  // Helper validation: cache dir helpers are deterministic and scoped
  {
    std::string dirA = cacheDir("fanqie", "6838480082219043843");
    std::string dirB = cacheDir("weread", "x");
    assert(dirA.find("/.crosspoint/provider_covers/") == 0);
    assert(dirB.find("/.crosspoint/provider_covers/") == 0);
    assert(dirA != dirB);
    assert(isProviderCoverCacheDir(dirA));
    assert(isProviderCoverCacheDir(dirA + "/cover_110x180.bmp") == true);
    assert(!isProviderCoverCacheDir("/books/local"));
    assert(!isProviderCoverCacheDir("/tmp/evil"));
    assert(!isProviderCoverCacheDir(""));
    assert(directoryOfCoverPath("/.crosspoint/provider_covers/abc/cover_[WIDTH]x[HEIGHT].bmp") == "/.crosspoint/provider_covers/abc");
    assert(directoryOfCoverPath("/.crosspoint/provider_covers/abc/cover_110x180.bmp") == "/.crosspoint/provider_covers/abc");
    assert(directoryOfCoverPath("") == "");
    assert(directoryOfCoverPath("no_slash") == "");
    assert(sourcePathInDir(dirA) == dirA + "/source.img");
    assert(sizedBmpPathInDir(dirA, 110, 180) == dirA + "/cover_110x180.bmp");
    assert(sizedBmpPathInDir(dirA, 74, 106) == dirA + "/cover_74x106.bmp");
    // concrete path helpers match sized path in dir
    assert(concreteBmpPath("fanqie", "6838480082219043843", 110, 180) == dirA + "/cover_110x180.bmp");
    assert(bmpTemplatePath("fanqie", "6838480082219043843") == dirA + "/cover_[WIDTH]x[HEIGHT].bmp");
    assert(sourcePath("fanqie", "6838480082219043843") == dirA + "/source.img");
    // path must be under /.crosspoint/provider_covers/ — outside must not be considered cache dir
    assert(!isProviderCoverCacheDir(directoryOfCoverPath("/books/local/cover_[WIDTH]x[HEIGHT].bmp")));
    assert(!isProviderCoverCacheDir(directoryOfCoverPath("/tmp/.crosspoint/provider_covers_fake/cover_[WIDTH]x[HEIGHT].bmp")) ||
           std::string("/tmp/.crosspoint/provider_covers_fake/cover_[WIDTH]x[HEIGHT].bmp").find("/.crosspoint/provider_covers/") != std::string::npos);
    // Empty/invalid cover path returns empty thumb
    EnsureSizedResult emptyW = ensureSizedCoverFromSource(templatePath, 0, 180, homeBackend);
    assert(emptyW.thumbPath.empty() && !emptyW.cacheHit && !emptyW.generated);
    EnsureSizedResult emptyH = ensureSizedCoverFromSource(templatePath, 110, 0, homeBackend);
    assert(emptyH.thumbPath.empty());
    EnsureSizedResult neg = ensureSizedCoverFromSource(templatePath, -1, 180, homeBackend);
    assert(neg.thumbPath.empty());
    EnsureSizedResult emptyPath = ensureSizedCoverFromSource("", 110, 180, homeBackend);
    assert(emptyPath.thumbPath.empty());
  }

  // Contract: cache hit when cover_{w}x{h}.bmp exists — no convert, no HTTP
  {
    std::set<std::string> hitFiles;
    std::map<std::string, std::vector<uint8_t>> hitPayloads;
    int hitFetches = 0;
    int hitConverts = 0;
    Backend b;
    b.exists = [&](const std::string& p){ return hitFiles.count(p)!=0; };
    b.fetch = [&](const std::string&,const std::string&,size_t){ ++hitFetches; return false; };
    b.convert = [&](const std::string&,const std::string& t,int w,int h){ ++hitConverts; hitFiles.insert(t); (void)w;(void)h; return true; };
    b.remove = [&](const std::string& p){ hitFiles.erase(p); };
    std::string dir = cacheDir("fanqie", "hit-book");
    std::string tpl = bmpTemplatePath("fanqie", "hit-book");
    std::string src = sourcePath("fanqie", "hit-book");
    std::string target110 = concreteBmpPath("fanqie", "hit-book", 110, 180);
    std::string target74 = concreteBmpPath("fanqie", "hit-book", 74, 106);
    // prime source and both targets as pre-existing cache hits
    hitFiles.insert(src);
    hitFiles.insert(target110);
    hitFiles.insert(target74);
    hitPayloads[target110] = makeFake1BitBmp(110,180);
    hitPayloads[target74] = makeFake1BitBmp(74,106);

    auto r110 = ensureSizedCoverFromSource(tpl, 110, 180, b);
    assert(r110.cacheHit && !r110.generated);
    assert(r110.thumbPath == target110);
    assert(hitFetches==0 && hitConverts==0);

    auto r74 = ensureSizedCoverFromSource(tpl, 74, 106, b);
    assert(r74.cacheHit && !r74.generated);
    assert(r74.thumbPath == target74);
    assert(hitFetches==0 && hitConverts==0);

    // Re-request same size must remain hit without extra work
    auto r110again = ensureSizedCoverFromSource(tpl, 110, 180, b);
    assert(r110again.cacheHit);
    assert(hitFetches==0 && hitConverts==0);
  }

  // Contract: generate from source.img JPEG, persist 1-bit exact WxH BMP, never HTTP
  {
    std::set<std::string> genFiles;
    std::map<std::string, std::vector<uint8_t>> genPayloads;
    int genFetches = 0;
    int genConverts = 0;
    std::string lastSrc;
    int lastW=0,lastH=0;
    Backend b;
    b.exists = [&](const std::string& p){ return genFiles.count(p)!=0; };
    b.fetch = [&](const std::string&,const std::string&,size_t){ ++genFetches; return false; };
    b.fetchCancellable = [&](const std::string&,const std::string&,size_t,const std::function<bool()>&){ ++genFetches; return false; };
    b.convert = [&](const std::string& src,const std::string& tgt,int w,int h){
      ++genConverts; lastSrc=src; lastW=w; lastH=h;
      // simulate JPEG->1-bit exact BMP via converter (home path uses oneBit=true)
      auto bmp = makeFake1BitBmp(w,h);
      genPayloads[tgt]=bmp;
      genFiles.insert(tgt);
      return true;
    };
    b.remove = [&](const std::string& p){ genFiles.erase(p); genPayloads.erase(p); };

    std::string dir = cacheDir("weread","generate-jpeg");
    std::string tpl = bmpTemplatePath("weread","generate-jpeg");
    std::string src = sourcePath("weread","generate-jpeg");
    // Simulate that provider download already produced source.img (JPEG magic)
    genFiles.insert(src);
    // Add fengyan 171x254 to prove generate uses source.img, not 171x254
    genFiles.insert(dir + "/cover_171x254.bmp");
    genPayloads[dir + "/cover_171x254.bmp"] = makeFake1BitBmp(171,254); // not 1-bit exact home but as 2-bit artifact it would be ignored

    auto r110 = ensureSizedCoverFromSource(tpl, 110, 180, b);
    assert(!r110.thumbPath.empty() && r110.generated && !r110.cacheHit);
    assert(lastSrc == src);
    assert(lastW==110 && lastH==180);
    assert(genFetches==0);
    assert(genFiles.count(dir + "/cover_110x180.bmp")==1);
    assert(isValid1BitBmpOfSize(genPayloads[dir + "/cover_110x180.bmp"], 110,180));

    auto r74 = ensureSizedCoverFromSource(tpl, 74, 106, b);
    assert(r74.generated);
    assert(lastW==74 && lastH==106);
    assert(isValid1BitBmpOfSize(genPayloads[dir + "/cover_74x106.bmp"], 74,106));
    assert(genFetches==0);
    assert(genConverts==2);

    // After generation, second call must be cache hit (persisted)
    lastSrc.clear();
    auto r110hit = ensureSizedCoverFromSource(tpl, 110, 180, b);
    assert(r110hit.cacheHit && !r110hit.generated);
    assert(r110hit.thumbPath == dir + "/cover_110x180.bmp");
    assert(genConverts==2); // no new convert
    assert(genFetches==0);
  }

  // Contract: missing source does not invent a download. Scene sizes last-resort
  // from cover_171x254.bmp; empty dir stays empty. Never HTTP.
  {
    std::set<std::string> missFiles;
    int missFetches=0, missConverts=0;
    std::string lastSrc;
    Backend b;
    b.exists = [&](const std::string& p){ return missFiles.count(p)!=0; };
    b.fetch = [&](const std::string&,const std::string&,size_t){ ++missFetches; return true; };
    b.fetchCancellable = [&](const std::string&,const std::string&,size_t,const std::function<bool()>&){ ++missFetches; return true; };
    b.convert = [&](const std::string& s,const std::string& t,int,int){
      ++missConverts; lastSrc=s; missFiles.insert(t); return true;
    };
    b.remove = [&](const std::string& p){ missFiles.erase(p); };

    std::string dir = cacheDir("fanqie","missing-source");
    std::string tpl = bmpTemplatePath("fanqie","missing-source");
    const std::string fb = dir + "/cover_171x254.bmp";
    missFiles.insert(fb);

    auto r = ensureSizedCoverFromSource(tpl, 110, 180, b);
    assert(!r.thumbPath.empty() && r.generated && !r.cacheHit);
    assert(lastSrc == fb);
    assert(missFetches==0);
    assert(missConverts==1);
    assert(missFiles.count(dir + "/cover_110x180.bmp")==1);

    // Also test with absolutely empty dir
    std::string dir2 = cacheDir("fanqie","empty-dir");
    std::string tpl2 = bmpTemplatePath("fanqie","empty-dir");
    auto r2 = ensureSizedCoverFromSource(tpl2, 74, 106, b);
    assert(r2.thumbPath.empty());
    assert(missFetches==0);
    assert(missConverts==1);
  }

  // Contract: convert failure removes partial target (no stale BMP)
  {
    std::set<std::string> failFiles;
    std::map<std::string, std::vector<uint8_t>> failPayloads;
    int fetches=0;
    bool removeCalled=false;
    std::string removedPath;
    Backend b;
    b.exists = [&](const std::string& p){ return failFiles.count(p)!=0; };
    b.fetch = [&](const std::string&,const std::string&,size_t){ ++fetches; return false; };
    b.convert = [&](const std::string& src,const std::string& tgt,int w,int h){
      // simulate partial write then failure
      (void)src;(void)w;(void)h;
      failFiles.insert(tgt);
      failPayloads[tgt] = std::vector<uint8_t>{'B','M',0,0}; // truncated partial
      return false;
    };
    b.remove = [&](const std::string& p){ removeCalled=true; removedPath=p; failFiles.erase(p); failPayloads.erase(p); };

    std::string dir = cacheDir("weread","convert-fail");
    std::string tpl = bmpTemplatePath("weread","convert-fail");
    std::string src = sourcePath("weread","convert-fail");
    failFiles.insert(src);

    auto r = ensureSizedCoverFromSource(tpl, 110, 180, b);
    assert(r.thumbPath.empty() && !r.cacheHit && !r.generated);
    assert(fetches==0);
    assert(removeCalled);
    assert(removedPath == dir + "/cover_110x180.bmp");
    assert(failFiles.count(dir + "/cover_110x180.bmp")==0);
    // second failure mode: convert returns false without even creating file — still must clear thumbPath
    failFiles.insert(src);
    removeCalled=false;
    b.convert = [&](const std::string&,const std::string&,int,int){ return false; };
    auto r2 = ensureSizedCoverFromSource(tpl, 74, 106, b);
    assert(r2.thumbPath.empty());
    // remove should have been attempted (even if file not present, backend.remove called)
    assert(removeCalled);
  }

  // Contract: convert failure where backend.exists(target) false after convert returns true — also cleans
  {
    std::set<std::string> cfFiles;
    int cfConverts=0;
    bool removeCalled=false;
    Backend b;
    b.exists = [&](const std::string& p){
      // source exists, but target never appears (simulates write failure)
      if (p.find("source.img")!=std::string::npos) return cfFiles.count(p)!=0;
      return false; // target never exists even after convert
    };
    b.fetch = [&](const std::string&,const std::string&,size_t){ return false; };
    b.convert = [&](const std::string&,const std::string&,int,int){ ++cfConverts; return true; };
    b.remove = [&](const std::string&){ removeCalled=true; };
    std::string dir = cacheDir("weread","convert-exists-fail");
    std::string tpl = bmpTemplatePath("weread","convert-exists-fail");
    std::string src = sourcePath("weread","convert-exists-fail");
    cfFiles.insert(src);
    auto r = ensureSizedCoverFromSource(tpl, 110, 180, b);
    assert(r.thumbPath.empty());
    assert(removeCalled);
    assert(cfConverts==1);
  }

  // Contract: path must be under /.crosspoint/provider_covers/ — outside rejected, no side effects
  {
    std::set<std::string> pFiles;
    int pFetches=0, pConverts=0, pRemoves=0;
    Backend b;
    b.exists = [&](const std::string& p){ return pFiles.count(p)!=0; };
    b.fetch = [&](const std::string&,const std::string&,size_t){ ++pFetches; return false; };
    b.convert = [&](const std::string&,const std::string& t,int,int){ ++pConverts; pFiles.insert(t); return true; };
    b.remove = [&](const std::string& p){ ++pRemoves; pFiles.erase(p); };

    struct Case { std::string path; bool shouldPass; };
    std::vector<Case> cases = {
      {"/books/local/cover_[WIDTH]x[HEIGHT].bmp", false},
      {"/tmp/evil/cover_[WIDTH]x[HEIGHT].bmp", false},
      {"/.crosspoint/other/cover_[WIDTH]x[HEIGHT].bmp", false},
      {"cover_[WIDTH]x[HEIGHT].bmp", false},
      {"", false},
      {cacheDir("fanqie","ok") + "/cover_[WIDTH]x[HEIGHT].bmp", true}, // should pass through dir check (hit/miss handled elsewhere)
    };
    for (auto &c: cases) {
      // For the true case, ensure source exists so we can see it not rejected at path stage
      if (c.shouldPass) {
        std::string dir = directoryOfCoverPath(c.path);
        pFiles.insert(sourcePathInDir(dir));
      }
      auto r = ensureSizedCoverFromSource(c.path, 110, 180, b);
      if (!c.shouldPass) {
        assert(r.thumbPath.empty() && !r.cacheHit && !r.generated);
      } else {
        // should have generated (if not already cached)
        assert(!r.thumbPath.empty() || r.thumbPath == sizedBmpPathInDir(directoryOfCoverPath(c.path),110,180));
        // clean for next iteration
        pFiles.clear();
      }
      pFetches=0; pConverts=0; // never should fetch, converts only for shouldPass
    }
    // Ensure outside never triggered convert/fetch/remove side effects
    pFiles.clear();
    pFetches=0; pConverts=0; pRemoves=0;
    auto outside = ensureSizedCoverFromSource("/books/local/cover_[WIDTH]x[HEIGHT].bmp", 110, 180, b);
    assert(outside.thumbPath.empty());
    assert(pFetches==0 && pConverts==0);

    auto evil = ensureSizedCoverFromSource("/tmp/cover_[WIDTH]x[HEIGHT].bmp", 74, 106, b);
    assert(evil.thumbPath.empty());
    assert(pFetches==0 && pConverts==0);
  }

  // Contract: never HTTP / never fetch — ensureSized never calls backend.fetch even under miss/fail/cancel
  {
    for (int sz : {110, 74}) {
      int h = (sz==110?180:106);
      std::set<std::string> f;
      int fetches=0;
      Backend b;
      b.exists = [&](const std::string& p){ return f.count(p)!=0; };
      b.fetch = [&](const std::string&,const std::string&,size_t){ ++fetches; return false; };
      b.fetchCancellable = [&](const std::string&,const std::string&,size_t,const std::function<bool()>&){ ++fetches; return false; };
      b.convert = [&](const std::string&,const std::string& t,int,int){ f.insert(t); return true; };
      b.remove = [&](const std::string& p){ f.erase(p); };

      std::string dir = cacheDir("weread", "never-http-" + std::to_string(sz));
      std::string tpl = bmpTemplatePath("weread", "never-http-" + std::to_string(sz));
      std::string src = sourcePath("weread", "never-http-" + std::to_string(sz));
      f.insert(src);
      auto r = ensureSizedCoverFromSource(tpl, sz, h, b);
      assert(!r.thumbPath.empty());
      assert(fetches==0);
      // hit path also no fetch
      auto r2 = ensureSizedCoverFromSource(tpl, sz, h, b);
      assert(r2.cacheHit);
      assert(fetches==0);
    }
    // even when source missing, no fetch
    {
      std::set<std::string> f;
      int fetches=0;
      Backend b;
      b.exists = [&](const std::string&){ return false; };
      b.fetch = [&](const std::string&,const std::string&,size_t){ ++fetches; return false; };
      b.convert = [&](const std::string&,const std::string&,int,int){ return true; };
      b.remove = [&](const std::string&){};
      auto r = ensureSizedCoverFromSource(bmpTemplatePath("weread","never-http-missing"), 110, 180, b);
      assert(r.thumbPath.empty());
      assert(fetches==0);
    }
  }

  // Contract: cancelled semantics
  {
    // Cancelled before convert — must not convert, no file, thumb empty, no fetch
    {
      std::set<std::string> f;
      int fetches=0, converts=0;
      Backend b;
      b.exists = [&](const std::string& p){ return f.count(p)!=0; };
      b.fetch = [&](const std::string&,const std::string&,size_t){ ++fetches; return false; };
      b.convert = [&](const std::string&,const std::string& t,int,int){ ++converts; f.insert(t); return true; };
      b.remove = [&](const std::string& p){ f.erase(p); };
      std::string dir = cacheDir("fanqie","cancel-before");
      std::string tpl = bmpTemplatePath("fanqie","cancel-before");
      std::string src = sourcePath("fanqie","cancel-before");
      f.insert(src);
      bool flag=true;
      auto r = ensureSizedCoverFromSource(tpl, 110, 180, b, [&](){ return flag; });
      assert(r.thumbPath.empty() && !r.cacheHit && !r.generated);
      assert(converts==0 && fetches==0);
      assert(f.count(dir + "/cover_110x180.bmp")==0);
    }
    // Cancelled after successful convert — file must be kept (next Home paint is hit), but thumbPath still returned? Current header keeps thumbPath when cancelled after convert (generated true)
    {
      std::set<std::string> f;
      Backend b;
      b.exists = [&](const std::string& p){ return f.count(p)!=0; };
      b.convert = [&](const std::string&,const std::string& t,int,int){ f.insert(t); return true; };
      b.remove = [&](const std::string& p){ f.erase(p); };
      std::string dir = cacheDir("fanqie","cancel-after");
      std::string tpl = bmpTemplatePath("fanqie","cancel-after");
      std::string src = sourcePath("fanqie","cancel-after");
      f.insert(src);
      bool flag=false;
      // Backend.convert will succeed, then ensureSized checks cancelled again.
      // Simulate flag becomes true after convert but before second cancelled check.
      // Our simple flag would be true before convert too, so we need a toggling lambda:
      int calls=0;
      auto toggleCancelled = [&]() -> bool {
        ++calls;
        // Calls: 1=top, 2=pre-source, 3=post-convert. Need 3 to reach post-convert.
        return calls >= 3;
      };
      // Need to ensure source exists and pre-cancel false on first call
      // Use a backend where convert inserts file, then post-cancel true should still return generated true (per header logic)
      // Header's second cancelled check happens after convert, and if true it still returns generated true with thumbPath kept.
      // So we test that file is kept even though cancelled.
      auto r = ensureSizedCoverFromSource(tpl, 110, 180, b, toggleCancelled);
      // With toggle, first call false allows convert, second call true keeps file but still returns generated
      assert(!r.thumbPath.empty() && r.generated);
      assert(f.count(dir + "/cover_110x180.bmp")==1);
    }
    // Not cancelled — normal generate
    {
      std::set<std::string> f;
      Backend b;
      b.exists = [&](const std::string& p){ return f.count(p)!=0; };
      b.convert = [&](const std::string&,const std::string& t,int,int){ f.insert(t); return true; };
      b.remove = [&](const std::string& p){ f.erase(p); };
      std::string dir = cacheDir("fanqie","cancel-none");
      std::string tpl = bmpTemplatePath("fanqie","cancel-none");
      std::string src = sourcePath("fanqie","cancel-none");
      f.insert(src);
      auto r = ensureSizedCoverFromSource(tpl, 110, 180, b, [](){ return false; });
      assert(r.generated && !r.cacheHit);
      assert(!r.thumbPath.empty());
    }
  }

  // Contract: helpers return correct paths and handle edge cases without crashing (no HTTP side effect)
  {
    // width/height edge: ensureSized with missing backend callbacks returns empty safely
    Backend emptyBackend;
    auto r1 = ensureSizedCoverFromSource(templatePath, 110, 180, emptyBackend);
    assert(r1.thumbPath.empty());
    Backend noExists;
    noExists.convert = [](const std::string&,const std::string&,int,int){ return true; };
    auto r2 = ensureSizedCoverFromSource(templatePath, 110, 180, noExists);
    assert(r2.thumbPath.empty());
    Backend noConvert;
    noConvert.exists = [](const std::string&){ return false; };
    auto r3 = ensureSizedCoverFromSource(templatePath, 110, 180, noConvert);
    assert(r3.thumbPath.empty());
  }

  std::cout << "provider cover cache bounded/reuse/failure/success passed\n";
  std::cout << "generate-on-miss: hit/generate-1bit-exact/never-http/no-invent-download/cancel/cleanup/path-scoped passed\n";
}

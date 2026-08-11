#include "apps/M4xJsonStream.h"
#include "apps/native/M4NativeUi.h"
#include "apps/native/M4NativeUiController.h"
#include "apps/providers/M4NativeLoadUi.h"
#include "apps/providers/M4NativeProviderExplore.h"
#include "apps/providers/M4NovelProviderContract.h"
#include "util/M4ContentProviderContract.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

class FixtureController final : public M4NativeUi::Controller {
 public:
  bool scalar(const std::string& key, std::string& out) const override {
    if (key == "app.name") {
      out = "Reader";
      return true;
    }
    return false;
  }
};

class StringSink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    if (!data) return false;
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  std::string body;
};

void parseHappyPath() {
  const char* xml = R"XML(<?xml version="1.0"?>
<m4ui version="1" start="shelf" theme="wap" fullscreen="true" uiScale="82">
  <screen id="shelf" title="@app.name" class="compact">
    <text text="Native &amp; bounded" bold="true" class="hero center"/>
    <list id="books" source="provider.books" titleField="title" subtitleField="author" onActivate="provider.openBook" class="ranked inset"/>
    <progress source="@loading.percent" max="100"/>
    <buttons back="Back" primary="Open" onBack="system.back" onPrimary="provider.openSelected"/>
  </screen>
</m4ui>)XML";
  const auto r = M4NativeUi::parse(xml, std::char_traits<char>::length(xml));
  assert(r);
  assert(r.document.startScreen == "shelf");
  assert(r.document.theme == "wap");
  assert(r.document.fullscreen);
  assert(r.document.uiScalePercent == 82);
  const auto* s = M4NativeUi::findScreen(r.document, "shelf");
  assert(s != nullptr);
  assert(M4NativeUi::hasStyle(s->style, M4NativeUi::StyleCompact));
  assert(s->nodes.size() == 4);
  assert(s->nodes[0].text == "Native & bounded");
  assert(M4NativeUi::hasStyle(s->nodes[0].style, M4NativeUi::StyleHero));
  assert(M4NativeUi::hasStyle(s->nodes[0].style, M4NativeUi::StyleCenter));
  assert(s->nodes[1].source == "provider.books");
  assert(M4NativeUi::hasStyle(s->nodes[1].style, M4NativeUi::StyleRanked));
  assert(M4NativeUi::hasStyle(s->nodes[1].style, M4NativeUi::StyleInset));
  FixtureController c;
  assert(M4NativeUi::resolveText(c, s->title) == "Reader");

  const char* bounded = R"XML(<m4ui version="1" uiScale="500"><screen id="x"></screen></m4ui>)XML";
  const auto b = M4NativeUi::parse(bounded, std::char_traits<char>::length(bounded));
  assert(b);
  assert(!b.document.fullscreen);
  assert(b.document.uiScalePercent == 125);
}

void tilesComponent() {
  const char* xml = R"XML(<m4ui version="1" start="home"><screen id="home">
    <tiles id="categories" source="provider.categories" pageSize="8" height="116" onActivate="provider.selectCategory" class="inset compact"/>
    <list id="books" source="provider.recommend" onActivate="provider.openBook"/>
  </screen></m4ui>)XML";
  const auto r = M4NativeUi::parse(xml, std::char_traits<char>::length(xml));
  assert(r);
  const auto* s = M4NativeUi::findScreen(r.document, "home");
  assert(s && s->nodes.size() == 2);
  assert(s->nodes[0].type == M4NativeUi::NodeType::Tiles);
  assert(s->nodes[0].source == "provider.categories");
  assert(s->nodes[0].pageSize == 8);
  assert(s->nodes[0].height == 116);
  assert(s->nodes[0].action == "provider.selectCategory");
  assert(M4NativeUi::hasStyle(s->nodes[0].style, M4NativeUi::StyleInset));

  const char* bad = R"XML(<m4ui version="1"><screen id="x"><tiles id="t"/></screen></m4ui>)XML";
  assert(!M4NativeUi::parse(bad, std::char_traits<char>::length(bad)));
}

void exploreContract() {
  assert(M4NativeProviderExplore::count("jjwxc") == 8);
  assert(M4NativeProviderExplore::count("fanqie") == 8);
  M4NativeProviderExplore::Category c{};
  assert(M4NativeProviderExplore::at("jjwxc", 0, c));
  assert(std::string(c.title) == "古代言情");
  assert(M4NativeProviderExplore::find("fanqie", "1:516", c));
  assert(std::string(c.title) == "都市异能");
  int gender = 0;
  int categoryId = 0;
  assert(M4NativeProviderExplore::decodeFanqieKey("1:516", gender, categoryId));
  assert(gender == 1 && categoryId == 516);
  assert(!M4NativeProviderExplore::decodeFanqieKey("bad", gender, categoryId));

  M4NovelProvider::Descriptor d;
  d.providerId = "demo";
  assert(M4NovelProvider::has(d.capabilities, M4NovelProvider::CapabilityExplore));
  assert(M4NovelProvider::has(d.capabilities, M4NovelProvider::CapabilityBookDetail));
}

void loadingPresentation() {
  M4NativeLoadUi::Snapshot s;
  s.scope = M4NativeLoadUi::Scope::Catalog;
  s.stage = M4NativeLoadUi::Stage::Receiving;
  s.receivedBytes = 12u * 1024u;
  s.rows = 36;
  s.elapsedSeconds = 2;
  assert(M4NativeLoadUi::title(s) == "接收目录");
  assert(M4NativeLoadUi::detail(s).find("36 项") != std::string::npos);
  assert(std::string(M4NativeLoadUi::stageKey(s.stage)) == "receiving");
}

void numericEntities() {
  const char* xml = R"XML(<m4ui version="1"><screen id="x"><flowText text="A&#10;B&#x4E2D;"/></screen></m4ui>)XML";
  const auto r = M4NativeUi::parse(xml, std::char_traits<char>::length(xml));
  assert(r);
  const auto* s = M4NativeUi::findScreen(r.document, "x");
  assert(s && s->nodes.size() == 1);
  assert(s->nodes[0].text == std::string("A\nB中"));
}

void rootArrayRecords() {
  const std::string json =
      R"JSON([{"chapterid":"101","chaptername":"第一章","isvip":"0"},{"chapterid":"102","chaptername":"第二章","isvip":"1"}])JSON";
  std::string clean;
  clean.reserve(json.size());
  for (size_t i = 0; i < json.size(); ++i) {
    if (json[i] == '\\' && i + 1 < json.size() && json[i + 1] == '"') continue;
    clean.push_back(json[i]);
  }
  StringSink sink;
  M4xJsonStream::RecordExtractor rows({}, {"chapterid", "chaptername", "isvip"}, sink, 8);
  for (size_t off = 0; off < clean.size();) {
    const size_t n = std::min<size_t>(7, clean.size() - off);
    assert(rows.feed(reinterpret_cast<const uint8_t*>(clean.data() + off), n));
    off += n;
  }
  assert(rows.finish());
  assert(rows.recordCount() == 2);
  assert(sink.body == "101\t第一章\t0\n102\t第二章\t1\n");
}

void largeShelfRecords() {
  std::string json = "{\"books\":[";
  for (int i = 0; i < 257; ++i) {
    if (i) json.push_back(',');
    json += "{\"bookId\":\"" + std::to_string(i) + "\",\"title\":\"book\"}";
  }
  json += "]}";
  StringSink sink;
  M4xJsonStream::RecordExtractor rows({"books"}, {"bookId", "title"}, sink, 4096);
  for (size_t off = 0; off < json.size();) {
    const size_t n = std::min<size_t>(31, json.size() - off);
    assert(rows.feed(reinterpret_cast<const uint8_t*>(json.data() + off), n));
    off += n;
  }
  assert(rows.finish());
  assert(rows.recordCount() == 257);
}

void unknownStyleIsForwardCompatible() {
  const char* xml = R"XML(<m4ui version="1"><screen id="x"><text text="ok" class="future-token muted"/></screen></m4ui>)XML";
  const auto r = M4NativeUi::parse(xml, std::char_traits<char>::length(xml));
  assert(r);
  const auto* s = M4NativeUi::findScreen(r.document, "x");
  assert(s && s->nodes.size() == 1);
  assert(M4NativeUi::hasStyle(s->nodes[0].style, M4NativeUi::StyleMuted));
}

void rejectExecutableShape() {
  const char* xml = R"XML(<m4ui version="1"><screen id="x"><text>run()</text></screen></m4ui>)XML";
  const auto r = M4NativeUi::parse(xml, std::char_traits<char>::length(xml));
  assert(!r);
}

void rejectOversizeAndBadSource() {
  M4NativeUi::Limits lim;
  lim.maxBytes = 8;
  const char* xml = "<m4ui/>";
  assert(!M4NativeUi::parse(xml, std::char_traits<char>::length(xml), lim));

  const char* noSource = R"XML(<m4ui version="1"><screen id="x"><list id="l"/></screen></m4ui>)XML";
  assert(!M4NativeUi::parse(noSource, std::char_traits<char>::length(noSource)));
}

void providerContract() {
  const std::string uri = M4ContentProvider::makeHistoryUri("weread", "12345");
  assert(uri == "m4cp://weread/12345");
  std::string p, b;
  assert(M4ContentProvider::parseHistoryUri(uri.c_str(), p, b));
  assert(p == "weread" && b == "12345");
  assert(!M4ContentProvider::isSafeCacheRelPath("../secret.txt"));
  assert(M4ContentProvider::isSafeCacheRelPath("cache/123/ch_1.txt"));

  M4ContentProvider::ChapterStatus next;
  next.state = M4ContentProvider::ChapterReady::Missing;
  auto decision = M4ContentProvider::decideNextChapter(next, true);
  assert(decision.action == M4ContentProvider::NextChapterDecision::Action::RequestAndWait);
  assert(decision.shouldRequestPrefetch);
  assert(M4ContentProvider::shouldIdlePrefetchNext(next));

  next.state = M4ContentProvider::ChapterReady::Fetching;
  decision = M4ContentProvider::decideNextChapter(next, true);
  assert(decision.action == M4ContentProvider::NextChapterDecision::Action::WaitOverlay);
  assert(!M4ContentProvider::shouldIdlePrefetchNext(next));

  next.state = M4ContentProvider::ChapterReady::Error;
  decision = M4ContentProvider::decideNextChapter(next, true);
  assert(decision.action == M4ContentProvider::NextChapterDecision::Action::WaitOverlay);
  assert(decision.shouldRequestPrefetch);
  assert(!M4ContentProvider::shouldIdlePrefetchNext(next));

  next.state = M4ContentProvider::ChapterReady::Ready;
  next.cacheRelPath = "cache/123/ch_2.txt";
  decision = M4ContentProvider::decideNextChapter(next, true);
  assert(decision.action == M4ContentProvider::NextChapterDecision::Action::OpenReady);
}

}  // namespace

int main() {
  parseHappyPath();
  tilesComponent();
  exploreContract();
  loadingPresentation();
  numericEntities();
  rootArrayRecords();
  largeShelfRecords();
  unknownStyleIsForwardCompatible();
  rejectExecutableShape();
  rejectOversizeAndBadSource();
  providerContract();
  std::cout << "native-ui tests passed\n";
  return 0;
}
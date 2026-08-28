// Home critical-UI contract.
//
// This intentionally stays source-level: HomeActivity depends on the Arduino
// runtime, while the composition contract can still be checked on the host.
#include <assert.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

char* readFile(const char* path) {
  std::FILE* input = std::fopen(path, "rb");
  assert(input);
  std::fseek(input, 0, SEEK_END);
  const long size = std::ftell(input);
  std::rewind(input);
  char* source = static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1));
  assert(source);
  assert(std::fread(source, 1, static_cast<size_t>(size), input) == static_cast<size_t>(size));
  source[size] = '\0';
  std::fclose(input);
  return source;
}

void require(const char* source, const char* needle) {
  assert(std::strstr(source, needle) != nullptr && "Home critical-UI contract missing");
}

void preservesThemeOwnedRenderingAndBehavior(const char* source) {
  require(source, "GUI.drawRecentBookCover(");
  require(source, "GUI.drawButtonMenu(");
  require(source, "mappedInput.wasScreenTapped");
  require(source, "onSelectBook(b.path, src);");
  require(source, "renderer.displayBuffer(HalDisplay::FAST_REFRESH);");
}

void compositionUsesOneGeometryPolicy(const char* source) {
  require(source, "struct HomeCompositionLayout");
  require(source, "makeHomeCompositionLayout");
  require(source, "const auto homeLayout = makeHomeCompositionLayout(metrics, pageHeight);");
  const char* first = std::strstr(source, "const auto homeLayout = makeHomeCompositionLayout(metrics, pageHeight);");
  assert(std::strstr(first + 1, "const auto homeLayout = makeHomeCompositionLayout(metrics, pageHeight);") != nullptr);
  require(source, "drawHomeSectionRule(renderer, metrics, homeLayout, pageWidth);");

  const char* cover = std::strstr(source, "GUI.drawRecentBookCover(");
  const char* rule = std::strstr(source, "drawHomeSectionRule(renderer, metrics, homeLayout, pageWidth);");
  const char* menu = std::strstr(source, "GUI.drawButtonMenu(");
  assert(cover < rule && rule < menu);
}

void routesReferenceGeometryThroughTouch(const char* source) {
  require(source, "fengyanRecentBookIndexFromPoint");
  require(source, "kHomeQuickColumns");
}

void sectionRuleIsSparseAndScreenLocal(const char* source) {
  require(source, "kHomeSectionRuleGapPx");
  require(source, "metrics.contentSidePadding");
  require(source, "renderer.drawLine(ruleInset, ruleY, pageWidth - ruleInset - 1, ruleY, 1, true);");
  const char* helper = std::strstr(source, "drawHomeSectionRule");
  assert(std::strstr(helper, "fillRectDither") == nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2 && "usage: test_home_critical_ui PATH_TO_HOME_ACTIVITY_CPP");
  char* source = readFile(argv[1]);
  preservesThemeOwnedRenderingAndBehavior(source);
  compositionUsesOneGeometryPolicy(source);
  sectionRuleIsSparseAndScreenLocal(source);
  routesReferenceGeometryThroughTouch(source);
  std::free(source);
  std::puts("home critical UI contract passed");
  return 0;
}

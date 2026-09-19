#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ui/scene/UiSceneRuntime.h"

namespace {

using namespace UiScene;
using namespace UiSceneRuntime;

struct SceneBuilder {
  uint8_t scene[2048]{};
  uint8_t package[2304]{};
  size_t sceneSize = 8;
  uint16_t commandCount = 0;

  static void put16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
  }

  static void put32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
  }

  static size_t command(uint8_t* out, uint8_t type, uint8_t flags,
                        const uint8_t* payload, size_t payloadSize) {
    assert(payloadSize <= 0xFFFF);
    out[0] = type;
    out[1] = flags;
    put16(out + 2, static_cast<uint16_t>(payloadSize));
    if (payloadSize != 0) std::memcpy(out + 4, payload, payloadSize);
    const size_t total = (4 + payloadSize + 3) & ~size_t(3);
    std::memset(out + 4 + payloadSize, 0, total - 4 - payloadSize);
    return total;
  }

  void add(uint8_t type, uint8_t flags, const uint8_t* payload,
           size_t payloadSize) {
    const size_t written = command(scene + sceneSize, type, flags, payload, payloadSize);
    sceneSize += written;
    ++commandCount;
  }

  static size_t prefix(uint8_t* out, uint8_t flags, BindingId visible,
                       ActionId action, BindingId argument = kInvalidBindingId) {
    size_t size = 0;
    if ((flags & kFlagVisibleIf) != 0) out[size++] = visible;
    if ((flags & kFlagAction) != 0) {
      out[size++] = action;
      out[size++] = argument == kInvalidBindingId ? 0 : 1;
      if (argument != kInvalidBindingId) out[size++] = argument;
    }
    return size;
  }

  void addClear() {
    const uint8_t payload[] = {0};
    add(kNodeClear, 0, payload, sizeof(payload));
  }

  void addText(const char* literal, BindingId binding = kInvalidBindingId,
               uint8_t flags = 0, BindingId visible = kInvalidBindingId,
               ActionId action = kInvalidActionId,
               BindingId argument = kInvalidBindingId, uint16_t x = 0,
               uint16_t y = 0) {
    uint8_t payload[128]{};
    size_t size = prefix(payload, flags, visible, action, argument);
    put16(payload + size, x);
    put16(payload + size + 2, y);
    put16(payload + size + 4, 180);
    put16(payload + size + 6, 24);
    payload[size + 8] = 16;
    payload[size + 9] = 0;
    payload[size + 10] = 0;
    payload[size + 11] = 1;
    payload[size + 12] = binding == kInvalidBindingId ? 0 : 1;
    payload[size + 13] = binding == kInvalidBindingId ? 0 : binding;
    const size_t literalSize = binding == kInvalidBindingId ? std::strlen(literal) : 0;
    put16(payload + size + 14, static_cast<uint16_t>(literalSize));
    if (literalSize != 0) std::memcpy(payload + size + 16, literal, literalSize);
    add(kNodeText, flags, payload, size + 16 + literalSize);
  }

  void addBitmap() {
    uint8_t payload[32]{};
    put16(payload, 1);
    put16(payload + 2, 2);
    put16(payload + 4, 30);
    put16(payload + 6, 40);
    put16(payload + 8, 2);
    payload[10] = 'b';
    payload[11] = 'g';
    add(kNodeBitmap, 0, payload, 12);
  }

  void addLine() {
    uint8_t payload[10]{};
    put16(payload, 2);
    put16(payload + 2, 50);
    put16(payload + 4, 30);
    put16(payload + 6, 50);
    payload[8] = 1;
    payload[9] = 1;
    add(kNodeLine, 0, payload, sizeof(payload));
  }

  void addRectNode(uint8_t type, BindingId binding, uint8_t flags = 0,
                  BindingId visible = kInvalidBindingId,
                  ActionId action = kInvalidActionId,
                  BindingId argument = kInvalidBindingId, uint16_t x = 0,
                  uint16_t y = 0) {
    uint8_t payload[32]{};
    size_t size = prefix(payload, flags, visible, action, argument);
    put16(payload + size, x);
    put16(payload + size + 2, y);
    put16(payload + size + 4, 80);
    put16(payload + size + 6, 60);
    if (type == kNodeCover) {
      put16(payload + size + 8, 4);
      payload[size + 10] = 1;
      payload[size + 11] = binding;
      add(type, flags, payload, size + 12);
    } else if (type == kNodeProgress) {
      put16(payload + size + 8, 4);
      payload[size + 10] = binding;
      add(type, flags, payload, size + 11);
    } else {
      payload[size + 8] = binding;
      add(type, flags, payload, size + 9);
    }
  }

  void addRepeat(BindingId source, uint8_t limit, uint16_t x, uint16_t y,
                 uint16_t itemWidth, uint16_t itemHeight, uint16_t gap,
                 const uint8_t* children, size_t childrenSize,
                 uint16_t childCount) {
    uint8_t payload[512]{};
    payload[0] = source;
    payload[1] = limit;
    put16(payload + 2, x);
    put16(payload + 4, y);
    put16(payload + 6, itemWidth);
    put16(payload + 8, itemHeight);
    put16(payload + 10, gap);
    payload[12] = 0;
    payload[13] = 0;
    put16(payload + 14, childCount);
    std::memcpy(payload + 16, children, childrenSize);
    add(kNodeRepeat, 0, payload, 16 + childrenSize);
  }

  size_t finish() {
    put16(scene, 1);
    put16(scene + 2, commandCount);
    put16(scene + 4, 0);
    put16(scene + 6, 0);
    std::memset(package, 0, sizeof(package));
    put32(package, kM4thMagic);
    put16(package + 4, kM4thVersion);
    put16(package + 6, kM4thHeaderSize);
    put16(package + 12, kScreenWidth);
    put16(package + 14, kScreenHeight);
    put16(package + 16, 1);
    put32(package + 32, kSceneSection);
    put32(package + 40, 80);
    put32(package + 44, static_cast<uint32_t>(sceneSize));
    put32(package + 48, commandCount);
    std::memcpy(package + 80, scene, sceneSize);
    put32(package + 8, static_cast<uint32_t>(80 + sceneSize));
    return 80 + sceneSize;
  }
};

struct Model {
  DataState state = DataState::Ready;
  bool currentExists = true;
  int32_t battery = 73;
  int32_t progress = 35;
  const char* currentTitle = "Current Book";
  const char* currentCover = "/cover/current.bmp";
  const char* recentTitles[3] = {"Recent A", "Recent B", "Recent C"};
  const char* recentCovers[3] = {"/recent/a.bmp", "/recent/b.bmp", "/recent/c.bmp"};
  const char* appIds[4] = {"files", "weread", "fanqie", "jinjiang"};
  const char* appNames[4] = {"Files", "WeRead", "Fanqie", "Jinjiang"};
  const char* appIcons[4] = {"folder", "weread", "tomato", "jinjiang"};
  bool dynamicAvailable = true;
  int forbiddenBackendCalls = 0;
};

static TextView textView(const char* value) {
  return value ? TextView::fromRam(value, static_cast<uint16_t>(std::strlen(value)))
               : TextView{};
}

static bool resolve(const void* user, BindingId binding, const SceneItemContext* item,
                    ResolvedValue* out) {
  const Model* model = static_cast<const Model*>(user);
  *out = ResolvedValue{};
  if (!model->dynamicAvailable) return false;
  if (binding == 1) {
    out->kind = ValueKind::Int;
    out->number = model->battery;
    return true;
  }
  if (binding == 10) {
    out->kind = ValueKind::Bool;
    out->boolean = model->currentExists;
    return true;
  }
  if (!model->currentExists && (binding == 11 || binding == 14 || binding == 15)) return false;
  if (binding == 11) {
    out->kind = ValueKind::Text;
    out->text = textView(model->currentTitle);
    return true;
  }
  if (binding == 14) {
    out->kind = ValueKind::Text;
    out->text = textView(model->currentCover);
    return true;
  }
  if (binding == 15) {
    out->kind = ValueKind::Int;
    out->number = model->progress;
    return true;
  }
  if (!item || !item->valid) return false;
  if (item->sourceBinding == 20 && item->index < 3) {
    if (binding == 32) {
      out->kind = ValueKind::Text;
      out->text = textView(model->recentTitles[item->index]);
      return true;
    }
    if (binding == 33) {
      out->kind = ValueKind::Text;
      out->text = textView(model->recentCovers[item->index]);
      return true;
    }
  }
  if (item->sourceBinding == 21 && item->index < 4) {
    if (binding == 30) {
      out->kind = ValueKind::Text;
      out->text = textView(model->appIds[item->index]);
      return true;
    }
    if (binding == 31) {
      out->kind = ValueKind::Text;
      out->text = textView(model->appNames[item->index]);
      return true;
    }
    if (binding == 34) {
      out->kind = ValueKind::Text;
      out->text = textView(model->appIcons[item->index]);
      return true;
    }
  }
  return false;
}

static uint8_t count(const void*, BindingId source) {
  return source == 20 ? 3 : source == 21 ? 4 : 0;
}

struct Captured {
  RenderEvent event[64]{};
  size_t count = 0;
};

static void emit(void* user, const RenderEvent& event) {
  Captured* captured = static_cast<Captured*>(user);
  assert(captured->count < 64);
  captured->event[captured->count++] = event;
}

static bool equals(TextView value, const char* expected) {
  const size_t length = std::strlen(expected);
  if (!value.data || value.size != length) return false;
  for (size_t i = 0; i < length; ++i) {
    if (value.readByte(static_cast<uint16_t>(i)) != static_cast<uint8_t>(expected[i])) return false;
  }
  return true;
}

static SceneBuilder makeScene() {
  SceneBuilder builder;
  builder.addClear();
  builder.addText("Shell", kInvalidBindingId, 0, kInvalidBindingId,
                  kInvalidActionId, kInvalidBindingId, 10, 10);
  builder.addBitmap();
  builder.addLine();
  builder.addRectNode(kNodeBattery, 1, 0, kInvalidBindingId,
                      kInvalidActionId, kInvalidBindingId, 10, 50);
  builder.addText("", 11, 0, kInvalidBindingId, kInvalidActionId,
                  kInvalidBindingId, 10, 120);
  builder.addRectNode(kNodeCover, 14, kFlagAction, kInvalidBindingId, 0,
                      kInvalidBindingId, 220, 100);
  builder.addRectNode(kNodeProgress, 15, 0, kInvalidBindingId,
                      kInvalidActionId, kInvalidBindingId, 220, 170);
  builder.addText("Current", kInvalidBindingId, kFlagVisibleIf, 10,
                  kInvalidActionId, kInvalidBindingId, 10, 200);
  builder.addText("Back", kInvalidBindingId, kFlagAction,
                  kInvalidBindingId, 1, kInvalidBindingId, 10, 740);

  uint8_t recentChildren[256]{};
  uint8_t childPayload[64]{};
  size_t childSize = 0;
  builder.prefix(childPayload, 0, 0, kInvalidActionId);
  SceneBuilder::put16(childPayload, 0);
  SceneBuilder::put16(childPayload + 2, 0);
  SceneBuilder::put16(childPayload + 4, 80);
  SceneBuilder::put16(childPayload + 6, 80);
  SceneBuilder::put16(childPayload + 8, 2);
  childPayload[10] = 1;
  childPayload[11] = 33;
  childSize += SceneBuilder::command(recentChildren + childSize, kNodeCover, 0,
                                     childPayload, 12);
  size_t textSize = 0;
  textSize = SceneBuilder::prefix(childPayload, 0, 0, kInvalidActionId);
  SceneBuilder::put16(childPayload + textSize, 0);
  SceneBuilder::put16(childPayload + textSize + 2, 84);
  SceneBuilder::put16(childPayload + textSize + 4, 100);
  SceneBuilder::put16(childPayload + textSize + 6, 20);
  childPayload[textSize + 8] = 16;
  childPayload[textSize + 9] = 0;
  childPayload[textSize + 10] = 0;
  childPayload[textSize + 11] = 1;
  childPayload[textSize + 12] = 1;
  childPayload[textSize + 13] = 32;
  SceneBuilder::put16(childPayload + textSize + 14, 0);
  childSize += SceneBuilder::command(recentChildren + childSize, kNodeText, 0,
                                     childPayload, textSize + 16);
  builder.addRepeat(20, 3, 20, 300, 100, 110, 10, recentChildren, childSize, 2);

  uint8_t appChildren[256]{};
  size_t appChildSize = 0;
  size_t iconSize = SceneBuilder::prefix(childPayload, kFlagAction, 0, 3, 30);
  SceneBuilder::put16(childPayload + iconSize, 0);
  SceneBuilder::put16(childPayload + iconSize + 2, 0);
  SceneBuilder::put16(childPayload + iconSize + 4, 60);
  SceneBuilder::put16(childPayload + iconSize + 6, 60);
  SceneBuilder::put16(childPayload + iconSize + 8, 4);
  childPayload[iconSize + 10] = 'i';
  childPayload[iconSize + 11] = 'c';
  childPayload[iconSize + 12] = 'o';
  childPayload[iconSize + 13] = 'n';
  childPayload[iconSize + 14] = 1;
  childPayload[iconSize + 15] = 34;
  appChildSize += SceneBuilder::command(appChildren + appChildSize, kNodeIcon,
                                         kFlagAction, childPayload, iconSize + 16);
  textSize = SceneBuilder::prefix(childPayload, 0, 0, kInvalidActionId);
  SceneBuilder::put16(childPayload + textSize, 0);
  SceneBuilder::put16(childPayload + textSize + 2, 68);
  SceneBuilder::put16(childPayload + textSize + 4, 100);
  SceneBuilder::put16(childPayload + textSize + 6, 20);
  childPayload[textSize + 8] = 16;
  childPayload[textSize + 9] = 0;
  childPayload[textSize + 10] = 0;
  childPayload[textSize + 11] = 1;
  childPayload[textSize + 12] = 1;
  childPayload[textSize + 13] = 31;
  SceneBuilder::put16(childPayload + textSize + 14, 0);
  appChildSize += SceneBuilder::command(appChildren + appChildSize, kNodeText, 0,
                                         childPayload, textSize + 16);
  builder.addRepeat(21, 4, 20, 500, 100, 100, 10, appChildren, appChildSize, 2);
  builder.addText("Home", kInvalidBindingId, kFlagAction,
                  kInvalidBindingId, 2, kInvalidBindingId, 300, 740);
  builder.finish();
  return builder;
}

void testRenderAndHitTest() {
  SceneBuilder scene = makeScene();
  const size_t sceneLength = scene.finish();
  Model model;
  const SceneBindingSource source{&model, &resolve, &count};
  Captured captured;
  const SceneRenderSink sink{&captured, &emit};
  assert(renderScene(scene.package, sceneLength, source, sink));

  const uint8_t expected[] = {kNodeClear, kNodeText, kNodeBitmap, kNodeLine,
                              kNodeBattery, kNodeText, kNodeCover, kNodeProgress,
                              kNodeText, kNodeText, kNodeCover, kNodeText,
                              kNodeCover, kNodeText, kNodeCover, kNodeText,
                              kNodeIcon, kNodeText, kNodeIcon, kNodeText,
                              kNodeIcon, kNodeText, kNodeIcon, kNodeText,
                              kNodeText};
  assert(captured.count == sizeof(expected));
  for (size_t i = 0; i < captured.count; ++i) assert(captured.event[i].type == expected[i]);

  bool battery = false;
  bool progress = false;
  bool title = false;
  bool cover = false;
  int recent = 0;
  int apps = 0;
  for (size_t i = 0; i < captured.count; ++i) {
    const RenderEvent& event = captured.event[i];
    if (event.type == kNodeBattery) { assert(event.value == 73); battery = true; }
    if (event.type == kNodeProgress) { assert(event.value == 35); progress = true; }
    if (event.type == kNodeText && event.rect.y == 120) {
      assert(equals(event.text, "Current Book")); title = true;
    }
    if (event.type == kNodeCover && event.rect.x == 220) {
      assert(equals(event.text, "/cover/current.bmp"));
      assert(event.assetBinding == 14);
      cover = true;
    }
    if (event.type == kNodeCover && event.item.valid) {
      ++recent;
      assert(event.item.sourceBinding == 20);
      assert(event.assetBinding == 33);
    }
    if (event.type == kNodeIcon && event.item.valid) {
      ++apps;
      assert(event.item.sourceBinding == 21);
      assert(event.assetBinding == 34);
      assert(event.action == 3);
      assert(equals(event.argument, model.appIds[event.item.index]));
    }
  }
  assert(battery && progress && title && cover && recent == 3 && apps == 4);

  HitResult hit{};
  assert(hitTestScene(scene.package, sceneLength, source, 230, 110, &hit));
  assert(hit.action == 0 && !hit.item.valid);
  assert(hitTestScene(scene.package, sceneLength, source, 250, 510, &hit));
  assert(hit.action == 3 && hit.item.valid && hit.item.index == 2);
  assert(equals(hit.argument, "fanqie"));
  assert(hitTestScene(scene.package, sceneLength, source, 310, 750, &hit));
  assert(hit.action == 2 && !hit.item.valid);
}

void testMissingDynamicValuesKeepStaticShell() {
  SceneBuilder scene = makeScene();
  const size_t sceneLength = scene.finish();
  const DataState states[] = {DataState::Loading, DataState::Error};
  for (const DataState state : states) {
    Model model;
    model.state = state;
    model.currentExists = false;
    model.dynamicAvailable = false;
    const SceneBindingSource source{&model, &resolve, &count};
    Captured captured;
    const SceneRenderSink sink{&captured, &emit};
    assert(renderScene(scene.package, sceneLength, source, sink));

    bool shell = false;
    bool bitmap = false;
    int navigationActions = 0;
    for (size_t i = 0; i < captured.count; ++i) {
      const RenderEvent& event = captured.event[i];
      if (event.type == kNodeText && event.rect.x == 10 && event.rect.y == 10)
        shell = true;
      if (event.type == kNodeBitmap) bitmap = true;
      if (event.action == 1 || event.action == 2) ++navigationActions;
      assert(event.type != kNodeBattery);
      assert(event.type != kNodeProgress);
      assert(!(event.type == kNodeText && event.rect.y == 120));
      assert(!(event.type == kNodeText && event.rect.y == 200));
      assert(!(event.type == kNodeCover && event.rect.x == 220));
    }
    assert(shell && bitmap && navigationActions == 2);
    assert(model.forbiddenBackendCalls == 0);

    HitResult hit{};
    assert(!hitTestScene(scene.package, sceneLength, source, 230, 110, &hit));
    assert(hitTestScene(scene.package, sceneLength, source, 310, 750, &hit));
    assert(hit.action == 2 && !hit.item.valid);
  }
}

}  // namespace

int main() {
  testRenderAndHitTest();
  testMissingDynamicValuesKeepStaticShell();
  return 0;
}

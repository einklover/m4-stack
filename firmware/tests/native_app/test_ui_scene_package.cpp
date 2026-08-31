#include <cassert>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "generated/murphy_default_m4theme.h"
#include "ui/scene/UiScenePackage.h"
#include "util/HomeSceneRuntime.h"

using namespace UiScene;

static_assert(std::is_same<UiScene::SceneNode,
                           HomeSceneRuntime::SceneCommand>::value,
              "HomeSceneRuntime must alias the canonical scene command type");

static void writeU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

static void writeU32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

int main() {
  static_assert(static_cast<uint8_t>(DataState::Loading) == 0);
  static_assert(static_cast<uint8_t>(DataState::Ready) == 1);
  static_assert(static_cast<uint8_t>(DataState::Stale) == 2);
  static_assert(static_cast<uint8_t>(DataState::Empty) == 3);
  static_assert(static_cast<uint8_t>(DataState::Error) == 4);
  static_assert(kMaxSceneNodes == 128);
  static_assert(kMaxBindings == 64);
  static_assert(kMaxActions == 32);
  static_assert(kMaxRepeatItems == 8);

  const Rect bounds{10, 20, 30, 40};
  assert(bounds.contains(10, 20));
  assert(bounds.contains(39, 59));
  assert(!bounds.contains(40, 60));

  const char ram[] = "RAM";
  const char flash[] PROGMEM = "ROM";
  const TextView ramText = TextView::fromRam(ram, 3);
  const TextView flashText = TextView::fromProgmem(flash, 3);
  assert(ramText.size == 3 && ramText.readByte(1) == 'A');
  assert(flashText.size == 3 && flashText.readByte(1) == 'O');

  const BindingId bindings[2] = {7, 9};
  const ActionId actions[2] = {3, 5};
  const ItemContext item{1, 4, bindings, 2};
  assert(item.index == 1 && item.count == 4);
  assert(item.bindings[0] == 7 && actions[1] == 5);

  const SceneNode nodes[2] = {
      {kNodeClear, 0, 0, nullptr},
      {kNodeText, kFlagAction, 2, reinterpret_cast<const uint8_t*>(ram)},
  };
  const NumericBinding numeric[2] = {{7, 42}, {9, -3}};
  const ActionTarget targets[1] = {{5, bounds, kInvalidBindingId}};
  const UiScenePackage package{
      DataState::Ready, nodes, 2, numeric, 2, targets, 1};
  assert(package.nodes[0].type == kNodeClear);
  assert(package.nodes[1].type == kNodeText);
  assert(package.bindings[0].value == 42);
  assert(package.actions[0].bounds.contains(12, 22));

  static_assert(kM4thMagic == HomeSceneRuntime::kMagic);
  static_assert(kM4thVersion == HomeSceneRuntime::kVersion);
  static_assert(kSceneSection == HomeSceneRuntime::kSectionScene);
  static_assert(kNodeRepeat == HomeSceneRuntime::kNodeRepeat);

  uint8_t bytes[4] = {0x34, 0x12, 0x78, 0x56};
  assert(readU16(bytes) == HomeSceneRuntime::readU16(bytes));
  assert(readU32(bytes) == HomeSceneRuntime::readU32(bytes));

  const uint8_t* theme = murphy_default_m4theme;
  const size_t themeSize = murphy_default_m4theme_len;
  assert(validatePackage(theme, themeSize));
  SceneHeader sceneHeader{};
  assert(parseSceneHeader(theme, themeSize, &sceneHeader));
  assert(sceneHeader.commandCount == 18);
  uint16_t visited = 0;
  assert(forEachCommand(theme, themeSize, [&](const SceneCommand&) {
    ++visited;
    return true;
  }));
  assert(visited == sceneHeader.commandCount);

  uint8_t malformed[sizeof(murphy_default_m4theme)];
  const uint16_t sectionCount = readU16(theme + 16);
  const size_t tableEnd = kM4thHeaderSize + static_cast<size_t>(sectionCount) * 24;
  size_t sceneDescriptor = 0;
  for (uint16_t i = 0; i < sectionCount; ++i) {
    const size_t descriptor = kM4thHeaderSize + static_cast<size_t>(i) * 24;
    if (readU32(theme + descriptor) == kSceneSection) sceneDescriptor = descriptor;
  }
  assert(sceneDescriptor != 0);
  const uint32_t sceneOffset = readU32(theme + sceneDescriptor + 8);
  const uint32_t sceneLength = readU32(theme + sceneDescriptor + 12);

  std::memcpy(malformed, theme, themeSize);
  writeU16(malformed + sceneOffset + 2, sceneHeader.commandCount + 1);
  assert(!parseSceneHeader(malformed, themeSize, &sceneHeader));
  assert(!forEachCommand(malformed, themeSize, [](const SceneCommand&) { return true; }));

  std::memcpy(malformed, theme, themeSize);
  writeU32(malformed + sceneDescriptor + 12, sceneLength - 1);
  assert(!forEachCommand(malformed, themeSize, [](const SceneCommand&) { return true; }));

  std::memcpy(malformed, theme, themeSize);
  writeU16(malformed + sceneOffset + 8 + 2, 0xFFFF);
  assert(!forEachCommand(malformed, themeSize, [](const SceneCommand&) { return true; }));

  std::memcpy(malformed, theme, themeSize);
  malformed[sceneOffset + 8 + 5] = 1;
  assert(!validatePackage(malformed, themeSize));
  assert(!forEachCommand(malformed, themeSize, [](const SceneCommand&) { return true; }));

  std::memcpy(malformed, theme, themeSize);
  writeU32(malformed + kM4thHeaderSize + 24, readU32(malformed + kM4thHeaderSize));
  assert(!validatePackage(malformed, themeSize));

  size_t firstNonEmpty = 0;
  size_t secondNonEmpty = 0;
  for (uint16_t i = 0; i < sectionCount; ++i) {
    const size_t descriptor = kM4thHeaderSize + static_cast<size_t>(i) * 24;
    if (readU32(theme + descriptor + 12) == 0) continue;
    if (!firstNonEmpty) firstNonEmpty = descriptor;
    else if (!secondNonEmpty) { secondNonEmpty = descriptor; break; }
  }
  assert(firstNonEmpty && secondNonEmpty);
  std::memcpy(malformed, theme, themeSize);
  writeU32(malformed + secondNonEmpty + 8, readU32(malformed + firstNonEmpty + 8));
  assert(!validatePackage(malformed, themeSize));

  std::memcpy(malformed, theme, themeSize);
  writeU32(malformed + firstNonEmpty + 8, static_cast<uint32_t>(tableEnd - 4));
  assert(!validatePackage(malformed, themeSize));

  std::memcpy(malformed, theme, themeSize);
  writeU32(malformed + firstNonEmpty + 8, 0);
  writeU32(malformed + firstNonEmpty + 12, 0);
  assert(validatePackage(malformed, themeSize));

  const SceneCommand nullPayload{kNodeText, kFlagVisibleIf | kFlagAction, 4, nullptr};
  assert(sceneCommandVisibleBinding(nullPayload) == kInvalidBindingId);
  assert(sceneCommandActionId(nullPayload) == kInvalidActionId);
  assert(!sceneCommandActionHasArg(nullPayload));
  assert(sceneCommandActionArgBinding(nullPayload) == kInvalidBindingId);

  uint8_t visibleOnly[1] = {7};
  const SceneCommand truncatedAction{kNodeText, kFlagVisibleIf | kFlagAction, 1, visibleOnly};
  assert(sceneCommandVisibleBinding(truncatedAction) == 7);
  assert(sceneCommandActionId(truncatedAction) == kInvalidActionId);
  assert(!sceneCommandActionHasArg(truncatedAction));

  uint8_t truncatedArg[2] = {3, 1};
  const SceneCommand truncatedArgAction{kNodeText, kFlagAction, sizeof(truncatedArg), truncatedArg};
  assert(sceneCommandActionId(truncatedArgAction) == 3);
  assert(!sceneCommandActionHasArg(truncatedArgAction));
  assert(sceneCommandActionArgBinding(truncatedArgAction) == kInvalidBindingId);

  const SceneCommand nullRepeat{kNodeRepeat, 0, 16, nullptr};
  RepeatInfo repeat{};
  assert(!parseRepeatInfo(nullRepeat, &repeat));
  assert(!forEachRepeatChild(nullRepeat, [](const SceneCommand&) { return true; }));
  uint8_t shortRepeat[15]{};
  const SceneCommand truncatedRepeat{kNodeRepeat, 0, sizeof(shortRepeat), shortRepeat};
  assert(!parseRepeatInfo(truncatedRepeat, &repeat));
  return 0;
}

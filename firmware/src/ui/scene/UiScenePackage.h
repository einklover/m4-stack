#pragma once

#include "ui/scene/UiSceneTypes.h"

namespace UiScene {

static constexpr uint32_t kM4thMagic = 0x4854344D;
static constexpr uint16_t kM4thVersion = 1;
static constexpr uint16_t kM4thHeaderSize = 32;
static constexpr uint16_t kScreenWidth = 480;
static constexpr uint16_t kScreenHeight = 800;
static constexpr uint32_t kMaxPackageBytes = 256u * 1024u;
static constexpr uint16_t kSceneVersion = 1;

static constexpr uint32_t kMetaSection = 1;
static constexpr uint32_t kStringsSection = 2;
static constexpr uint32_t kSlotsSection = 3;
static constexpr uint32_t kAssetsSection = 4;
static constexpr uint32_t kAssetDataSection = 5;
static constexpr uint32_t kSceneSection = 6;
static constexpr uint32_t kInteractionsSection = 7;

static constexpr uint8_t kNodeClear = 0;
static constexpr uint8_t kNodeBitmap = 1;
static constexpr uint8_t kNodeLine = 2;
static constexpr uint8_t kNodeRect = 3;
static constexpr uint8_t kNodeRoundRect = 4;
static constexpr uint8_t kNodeText = 5;
static constexpr uint8_t kNodeCover = 6;
static constexpr uint8_t kNodeProgress = 7;
static constexpr uint8_t kNodeIcon = 8;
static constexpr uint8_t kNodeBattery = 9;
static constexpr uint8_t kNodeGroup = 10;
static constexpr uint8_t kNodeRepeat = 11;
static constexpr uint8_t kFlagVisibleIf = 0x01;
static constexpr uint8_t kFlagAction = 0x02;

struct SectionInfo {
  uint32_t type = 0, flags = 0, offset = 0, length = 0, count = 0, crc = 0;
};

struct SceneHeader {
  uint16_t version = 0, commandCount = 0, flags = 0, reserved = 0;
};

struct SceneNode {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t payloadLen = 0;
  const uint8_t* payload = nullptr;
  uint32_t offset = 0;
};

using SceneCommand = SceneNode;

struct UiScenePackage {
  DataState state = DataState::Loading;
  const SceneNode* nodes = nullptr;
  uint16_t nodeCount = 0;
  const NumericBinding* bindings = nullptr;
  uint8_t bindingCount = 0;
  const ActionTarget* actions = nullptr;
  uint8_t actionCount = 0;
};

inline uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(pgm_read_byte(p)) |
         (static_cast<uint16_t>(pgm_read_byte(p + 1)) << 8);
}

inline uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(pgm_read_byte(p)) |
         (static_cast<uint32_t>(pgm_read_byte(p + 1)) << 8) |
         (static_cast<uint32_t>(pgm_read_byte(p + 2)) << 16) |
         (static_cast<uint32_t>(pgm_read_byte(p + 3)) << 24);
}

inline bool isValidM4TH(const uint8_t* data, size_t len) {
  if (!data || len < kM4thHeaderSize || readU32(data) != kM4thMagic) return false;
  if (readU16(data + 4) != kM4thVersion || readU16(data + 6) != kM4thHeaderSize) return false;
  const uint32_t total = readU32(data + 8);
  if (total != len || total > kMaxPackageBytes) return false;
  if (readU16(data + 12) != kScreenWidth || readU16(data + 14) != kScreenHeight) return false;
  const uint16_t count = readU16(data + 16);
  const size_t tableEnd = kM4thHeaderSize + static_cast<size_t>(count) * 24;
  if (count == 0 || count > 32 || tableEnd > len) return false;
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* descriptor = data + kM4thHeaderSize + static_cast<size_t>(i) * 24;
    const uint32_t type = readU32(descriptor);
    const uint32_t offset = readU32(descriptor + 8);
    const uint32_t length = readU32(descriptor + 12);
    if ((offset & 3u) || offset > len || length > len - offset) return false;
    if (length && offset < tableEnd) return false;
    for (uint16_t j = 0; j < i; ++j) {
      const uint8_t* previous = data + kM4thHeaderSize + static_cast<size_t>(j) * 24;
      if (readU32(previous) == type) return false;
      const uint32_t previousOffset = readU32(previous + 8);
      const uint32_t previousLength = readU32(previous + 12);
      if (length && previousLength && offset < previousOffset + previousLength &&
          previousOffset < offset + length) return false;
    }
  }
  return true;
}

inline bool findSection(const uint8_t* data, size_t len, uint32_t type, SectionInfo* out) {
  if (!out || !isValidM4TH(data, len)) return false;
  const uint16_t count = readU16(data + 16);
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* p = data + kM4thHeaderSize + static_cast<size_t>(i) * 24;
    if (readU32(p) != type) continue;
    *out = {readU32(p), readU32(p + 4), readU32(p + 8), readU32(p + 12),
            readU32(p + 16), readU32(p + 20)};
    return true;
  }
  return false;
}

inline bool parseSceneHeader(const uint8_t* data, size_t len, SceneHeader* out) {
  SectionInfo section{};
  if (!out || !findSection(data, len, kSceneSection, &section) || section.length < 8) return false;
  const uint8_t* p = data + section.offset;
  const SceneHeader header{readU16(p), readU16(p + 2), readU16(p + 4), readU16(p + 6)};
  if (header.version != kSceneVersion || header.commandCount != section.count ||
      header.commandCount > kMaxSceneNodes) return false;
  *out = header;
  return true;
}

inline bool hasSceneSection(const uint8_t* data, size_t len) {
  SectionInfo section{};
  return findSection(data, len, kSceneSection, &section);
}

inline bool sceneCommandHasVisibleIf(const SceneCommand& command) {
  return (command.flags & kFlagVisibleIf) != 0;
}

inline bool sceneCommandHasAction(const SceneCommand& command) {
  return (command.flags & kFlagAction) != 0;
}

inline BindingId sceneCommandVisibleBinding(const SceneCommand& command) {
  if (!sceneCommandHasVisibleIf(command) || !command.payload || command.payloadLen < 1)
    return kInvalidBindingId;
  return pgm_read_byte(command.payload);
}

inline ActionId sceneCommandActionId(const SceneCommand& command) {
  if (!sceneCommandHasAction(command) || !command.payload) return kInvalidActionId;
  const size_t offset = sceneCommandHasVisibleIf(command) ? 1 : 0;
  if (offset > command.payloadLen || command.payloadLen - offset < 2)
    return kInvalidActionId;
  return pgm_read_byte(command.payload + offset);
}

inline bool sceneCommandRepeatPayloadOffset(const SceneCommand& command, size_t* out) {
  if (!out || !command.payload) return false;
  size_t offset = sceneCommandHasVisibleIf(command) ? 1u : 0u;
  if (!sceneCommandHasAction(command)) {
    *out = offset;
    return offset <= command.payloadLen;
  }
  if (offset > command.payloadLen || command.payloadLen - offset < 2) return false;
  offset += 2;
  if (pgm_read_byte(command.payload + offset - 1) != 0) {
    if (offset >= command.payloadLen) return false;
    ++offset;
  }
  *out = offset;
  return true;
}

inline bool sceneCommandPaddingIsZero(const uint8_t* data, size_t payloadLen,
                                      size_t paddedLen) {
  for (size_t i = 4u + payloadLen; i < paddedLen; ++i) {
    if (pgm_read_byte(data + i) != 0) return false;
  }
  return true;
}

inline bool sceneCommandActionHasArg(const SceneCommand& command) {
  if (!sceneCommandHasAction(command) || !command.payload) return false;
  const size_t offset = sceneCommandHasVisibleIf(command) ? 1 : 0;
  if (offset > command.payloadLen || command.payloadLen - offset < 2) return false;
  return pgm_read_byte(command.payload + offset + 1) != 0 &&
         command.payloadLen - offset >= 3;
}

inline BindingId sceneCommandActionArgBinding(const SceneCommand& command) {
  if (!sceneCommandActionHasArg(command) || !command.payload) return kInvalidBindingId;
  const size_t offset = sceneCommandHasVisibleIf(command) ? 1 : 0;
  if (offset + 2 >= command.payloadLen) return kInvalidBindingId;
  return pgm_read_byte(command.payload + offset + 2);
}

template <typename Fn>
inline bool forEachCommand(const uint8_t* data, size_t len, Fn&& fn) {
  SectionInfo section{};
  SceneHeader header{};
  if (!findSection(data, len, kSceneSection, &section) || !parseSceneHeader(data, len, &header)) return false;
  size_t offset = static_cast<size_t>(section.offset) + 8;
  const size_t end = static_cast<size_t>(section.offset) + section.length;
  for (uint16_t i = 0; i < header.commandCount; ++i) {
    if (offset > end || end - offset < 4) return false;
    const uint16_t payloadLen = readU16(data + offset + 2);
    const size_t padded = (static_cast<size_t>(payloadLen) + 7u) & ~size_t(3u);
    if (padded > end - offset ||
        !sceneCommandPaddingIsZero(data + offset, payloadLen, padded)) return false;
    const SceneNode node{pgm_read_byte(data + offset), pgm_read_byte(data + offset + 1),
                         payloadLen, payloadLen ? data + offset + 4 : nullptr,
                         static_cast<uint32_t>(offset)};
    if (!fn(node)) return true;
    offset += padded;
  }
  return offset == end;
}

struct RepeatInfo {
  BindingId sourceBinding = 0;
  uint8_t limit = 0;
  uint16_t x = 0, y = 0, itemW = 0, itemH = 0, gap = 0;
  uint8_t direction = 0;
  uint16_t childCount = 0;
};

inline bool parseRepeatInfo(const SceneCommand& command, RepeatInfo* out) {
  size_t payloadOffset = 0;
  if (!out || command.type != kNodeRepeat || !sceneCommandRepeatPayloadOffset(command, &payloadOffset) ||
      command.payloadLen - payloadOffset < 16) return false;
  const uint8_t* payload = command.payload + payloadOffset;
  const uint8_t limit = pgm_read_byte(payload + 1);
  const uint8_t direction = pgm_read_byte(payload + 12);
  const uint8_t reserved = pgm_read_byte(payload + 13);
  const uint16_t childCount = readU16(payload + 14);
  const size_t innerLength = static_cast<size_t>(command.payloadLen) - payloadOffset - 16;
  if (limit == 0 || limit > kMaxRepeatItems || direction > 1 || reserved != 0 ||
      childCount == 0 || childCount > 16 || innerLength < static_cast<size_t>(childCount) * 4)
    return false;
  *out = {pgm_read_byte(payload), limit, readU16(payload + 2),
          readU16(payload + 4), readU16(payload + 6),
          readU16(payload + 8), readU16(payload + 10),
          direction, childCount};
  return true;
}

template <typename Fn>
inline bool forEachRepeatChild(const SceneCommand& repeat, Fn&& fn) {
  RepeatInfo info{};
  if (!parseRepeatInfo(repeat, &info)) return false;
  size_t payloadOffset = 0;
  if (!sceneCommandRepeatPayloadOffset(repeat, &payloadOffset)) return false;
  const uint8_t* inner = repeat.payload + payloadOffset + 16;
  const size_t innerLength = static_cast<size_t>(repeat.payloadLen) - payloadOffset - 16;
  size_t offset = 0;
  for (uint16_t i = 0; i < info.childCount; ++i) {
    if (offset > innerLength || innerLength - offset < 4) return false;
    const uint16_t payloadLen = readU16(inner + offset + 2);
    const size_t padded = (static_cast<size_t>(payloadLen) + 7u) & ~size_t(3u);
    if (padded > innerLength - offset ||
        !sceneCommandPaddingIsZero(inner + offset, payloadLen, padded)) return false;
    const SceneCommand child{pgm_read_byte(inner + offset), pgm_read_byte(inner + offset + 1),
                             payloadLen, payloadLen ? inner + offset + 4 : nullptr,
                             repeat.offset + static_cast<uint32_t>(payloadOffset + 16 + offset)};
    if (!fn(child)) return true;
    offset += padded;
  }
  return offset == innerLength;
}

template <typename Fn>
inline bool forEachCommandVoid(const uint8_t* data, size_t len, Fn&& fn) {
  return forEachCommand(data, len, [&](const SceneCommand& command) {
    fn(command);
    return true;
  });
}

inline bool validatePackage(const uint8_t* data, size_t len) {
  if (!isValidM4TH(data, len)) return false;
  SectionInfo scene{};
  if (!findSection(data, len, kSceneSection, &scene)) return true;
  return forEachCommand(data, len, [](const SceneCommand&) { return true; });
}

} // namespace UiScene

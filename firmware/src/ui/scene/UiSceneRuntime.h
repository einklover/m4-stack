#pragma once

#include <cstddef>
#include <cstdint>

#include "ui/scene/UiScenePackage.h"

// Snapshot-only scene execution. SceneBindingSource must point at an already
// stable snapshot; this header has no provider, filesystem, network, or
// renderer dependency.
namespace UiSceneRuntime {

using UiScene::ActionId;
using UiScene::BindingId;
using UiScene::DataState;
using UiScene::Rect;
using UiScene::SceneCommand;
using UiScene::TextView;

static constexpr uint8_t kInvalidItemIndex = 0xFF;

struct SceneItemContext {
  bool valid = false;
  BindingId sourceBinding = UiScene::kInvalidBindingId;
  uint8_t index = kInvalidItemIndex;
  uint8_t count = 0;
};

enum class ValueKind : uint8_t { Missing, Bool, Int, Text, Asset };

struct ResolvedValue {
  ValueKind kind = ValueKind::Missing;
  bool boolean = false;
  int32_t number = 0;
  TextView text{};
  uint8_t assetIndex = 0xFF; // valid when kind==Asset
};

struct SceneBindingSource {
  using ResolveFn = bool (*)(const void*, BindingId, const SceneItemContext*,
                             ResolvedValue*);
  using CountFn = uint8_t (*)(const void*, BindingId);

  const void* user = nullptr;
  ResolveFn resolve = nullptr;
  CountFn count = nullptr;

  bool read(BindingId binding, const SceneItemContext* item,
            ResolvedValue* out) const {
    return resolve != nullptr && out != nullptr &&
           resolve(user, binding, item, out);
  }

  uint8_t size(BindingId source) const {
    return count == nullptr ? 0 : count(user, source);
  }
};

struct RenderEvent {
  uint8_t type = 0;
  uint8_t flags = 0;
  Rect rect{};
  int16_t x2 = 0;
  int16_t y2 = 0;
  uint16_t radius = 0;
  uint8_t width = 0;
  uint8_t color = 0;
  uint8_t fill = 0;
  uint8_t font = 0;
  uint8_t style = 0;
  uint8_t align = 0;
  int32_t value = 0;
  TextView text{};
  SceneItemContext item{};
  ActionId action = UiScene::kInvalidActionId;
  TextView argument{};
  BindingId assetBinding = UiScene::kInvalidBindingId;
  uint8_t assetIndex = 0xFF; // 0xFF = no asset, otherwise index into UiSceneAssets
  bool hasAsset = false;
};

struct SceneRenderSink {
  using EmitFn = void (*)(void*, const RenderEvent&);

  void* user = nullptr;
  EmitFn emit = nullptr;

  bool write(const RenderEvent& event) const {
    if (emit == nullptr) return false;
    emit(user, event);
    return true;
  }
};

struct HitResult {
  bool hit = false;
  ActionId action = UiScene::kInvalidActionId;
  SceneItemContext item{};
  Rect rect{};
  TextView argument{};
};

namespace detail {

static constexpr uint8_t kMaxDepth = 3;

struct Prefix {
  size_t offset = 0;
  BindingId visibleBinding = UiScene::kInvalidBindingId;
  ActionId action = UiScene::kInvalidActionId;
  BindingId argumentBinding = UiScene::kInvalidBindingId;
  bool hasVisible = false;
  bool hasAction = false;
  bool hasArgument = false;
};

inline bool readU16At(const SceneCommand& command, size_t offset,
                      uint16_t* out) {
  if (!out || !command.payload || offset > command.payloadLen ||
      command.payloadLen - offset < 2) return false;
  *out = UiScene::readU16(command.payload + offset);
  return true;
}

inline bool readPrefix(const SceneCommand& command, Prefix* out) {
  if (!out || (!command.payload && command.payloadLen != 0)) return false;
  Prefix result{};
  if (UiScene::sceneCommandHasVisibleIf(command)) {
    if (result.offset >= command.payloadLen) return false;
    result.hasVisible = true;
    result.visibleBinding = pgm_read_byte(command.payload + result.offset++);
  }
  if (UiScene::sceneCommandHasAction(command)) {
    if (command.payloadLen - result.offset < 2) return false;
    result.hasAction = true;
    result.action = pgm_read_byte(command.payload + result.offset++);
    const uint8_t hasArgument = pgm_read_byte(command.payload + result.offset++);
    if (hasArgument > 1) return false;
    result.hasArgument = hasArgument != 0;
    if (result.hasArgument) {
      if (result.offset >= command.payloadLen) return false;
      result.argumentBinding = pgm_read_byte(command.payload + result.offset++);
    }
  }
  *out = result;
  return true;
}

inline Rect shifted(Rect rect, int16_t dx, int16_t dy) {
  rect.x = static_cast<int16_t>(rect.x + dx);
  rect.y = static_cast<int16_t>(rect.y + dy);
  return rect;
}

inline bool readRect(const SceneCommand& command, size_t offset, Rect* out,
                     int16_t dx, int16_t dy) {
  uint16_t x = 0, y = 0, width = 0, height = 0;
  if (!readU16At(command, offset, &x) ||
      !readU16At(command, offset + 2, &y) ||
      !readU16At(command, offset + 4, &width) ||
      !readU16At(command, offset + 6, &height) || !out)
    return false;
  *out = shifted(Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), width,
                      height},
                 dx, dy);
  return true;
}

inline bool readString(const SceneCommand& command, size_t offset,
                       TextView* out, size_t* next) {
  uint16_t length = 0;
  if (!readU16At(command, offset, &length) ||
      command.payloadLen - offset - 2 < length || !out || !next)
    return false;
  *out = TextView::fromProgmem(
      reinterpret_cast<const char*>(command.payload + offset + 2), length);
  *next = offset + 2 + length;
  return true;
}

inline bool visible(const Prefix& prefix, const SceneBindingSource& source,
                    const SceneItemContext& item) {
  if (!prefix.hasVisible) return true;
  ResolvedValue value{};
  return source.read(prefix.visibleBinding, item.valid ? &item : nullptr,
                     &value) &&
         value.kind == ValueKind::Bool && value.boolean;
}

inline bool actionArgument(const Prefix& prefix, const SceneBindingSource& source,
                           const SceneItemContext& item, TextView* out) {
  if (!out) return false;
  *out = TextView{};
  if (!prefix.hasArgument) return true;
  ResolvedValue value{};
  if (!source.read(prefix.argumentBinding, item.valid ? &item : nullptr,
                  &value) ||
      value.kind != ValueKind::Text)
    return false;
  *out = value.text;
  return true;
}

inline void fillMetadata(const Prefix& prefix, const SceneBindingSource& source,
                         const SceneItemContext& item, RenderEvent* event) {
  event->action = prefix.hasAction ? prefix.action : UiScene::kInvalidActionId;
  event->item = item;
  if (!actionArgument(prefix, source, item, &event->argument))
    event->action = UiScene::kInvalidActionId;
}

inline bool emitSimple(const Prefix& prefix, const SceneBindingSource& source,
                       const SceneItemContext& item, SceneRenderSink sink,
                       RenderEvent event) {
  fillMetadata(prefix, source, item, &event);
  return sink.write(event);
}

inline bool executeCommand(const SceneCommand& command,
                           const SceneBindingSource& source,
                           const SceneItemContext& item, int16_t dx, int16_t dy,
                           SceneRenderSink sink, uint8_t depth);

template <typename Visitor>
inline bool forEachEmbedded(const uint8_t* data, size_t length,
                            uint16_t count, uint32_t baseOffset,
                            Visitor&& visitor) {
  size_t offset = 0;
  for (uint16_t i = 0; i < count; ++i) {
    if (!data || offset > length || length - offset < 4) return false;
    const uint16_t payloadLen = UiScene::readU16(data + offset + 2);
    const size_t padded = (static_cast<size_t>(payloadLen) + 7u) & ~size_t(3u);
    if (padded > length - offset) return false;
    if (!UiScene::sceneCommandPaddingIsZero(data + offset, payloadLen, padded))
      return false;
    const SceneCommand child{
        pgm_read_byte(data + offset), pgm_read_byte(data + offset + 1),
        payloadLen, payloadLen ? data + offset + 4 : nullptr,
        baseOffset + static_cast<uint32_t>(offset)};
    if (!visitor(child)) return false;
    offset += padded;
  }
  return offset == length;
}

inline bool executeChildren(const SceneCommand& command, size_t offset,
                            uint16_t count, const SceneBindingSource& source,
                            const SceneItemContext& item, int16_t dx, int16_t dy,
                            SceneRenderSink sink, uint8_t depth) {
  if (depth >= kMaxDepth || offset > command.payloadLen ||
      command.payloadLen - offset < 2) return false;
  uint16_t childCount = UiScene::readU16(command.payload + offset);
  if (childCount != count) return false;
  const uint8_t* children = command.payload + offset + 2;
  const size_t length = command.payloadLen - offset - 2;
  bool ok = true;
  return forEachEmbedded(children, length, childCount, command.offset,
                         [&](const SceneCommand& child) {
                           if (!executeCommand(child, source, item, dx, dy,
                                                sink, depth + 1)) {
                             ok = false;
                             return false;
                           }
                           return true;
                         }) &&
         ok;
}

inline bool executeRepeat(const SceneCommand& command, const Prefix& prefix,
                          const SceneBindingSource& source,
                          const SceneItemContext& parent, SceneRenderSink sink,
                          uint8_t depth) {
  if (depth >= kMaxDepth) return false;
  size_t offset = prefix.offset;
  if (command.payloadLen - offset < 16) return false;
  const uint8_t* payload = command.payload + offset;
  const BindingId list = pgm_read_byte(payload);
  const uint8_t limit = pgm_read_byte(payload + 1);
  const uint16_t x = UiScene::readU16(payload + 2);
  const uint16_t y = UiScene::readU16(payload + 4);
  const uint16_t itemWidth = UiScene::readU16(payload + 6);
  const uint16_t itemHeight = UiScene::readU16(payload + 8);
  const uint16_t gap = UiScene::readU16(payload + 10);
  const uint8_t direction = pgm_read_byte(payload + 12);
  const uint8_t reserved = pgm_read_byte(payload + 13);
  const uint16_t childCount = UiScene::readU16(payload + 14);
  if (limit == 0 || limit > UiScene::kMaxRepeatItems || direction > 1 ||
      reserved != 0 || childCount == 0 || childCount > 16)
    return false;
  const size_t childOffset = offset + 16;
  const size_t childLength = command.payloadLen - childOffset;
  const uint8_t count = source.size(list);
  const uint8_t itemCount = count < limit ? count : limit;
  bool ok = true;
  for (uint8_t index = 0; index < itemCount && ok; ++index) {
    SceneItemContext item{true, list, index, count};
    const int16_t itemX = static_cast<int16_t>(x +
        (direction == 0 ? index * (itemWidth + gap) : 0));
    const int16_t itemY = static_cast<int16_t>(y +
        (direction == 1 ? index * (itemHeight + gap) : 0));
    ok = forEachEmbedded(command.payload + childOffset, childLength,
                          childCount, command.offset,
                          [&](const SceneCommand& child) {
                            return executeCommand(child, source, item, itemX,
                                                   itemY, sink, depth + 1);
                          });
  }
  (void)parent;
  return ok;
}

inline bool executeCommand(const SceneCommand& command,
                           const SceneBindingSource& source,
                           const SceneItemContext& item, int16_t dx, int16_t dy,
                           SceneRenderSink sink, uint8_t depth) {
  Prefix prefix{};
  if (!readPrefix(command, &prefix) ||
      !visible(prefix, source, item)) return true;

  if (command.type == UiScene::kNodeRepeat) {
    return executeRepeat(command, prefix, source, item, sink, depth);
  }
  if (command.type == UiScene::kNodeGroup) {
    if (command.payloadLen - prefix.offset < 2) return false;
    const uint16_t childCount =
        UiScene::readU16(command.payload + prefix.offset);
    return executeChildren(command, prefix.offset, childCount, source, item,
                           dx, dy, sink, depth);
  }

  RenderEvent event{};
  event.type = command.type;
  event.flags = command.flags;
  size_t offset = prefix.offset;
  if (command.type == UiScene::kNodeClear) {
    if (command.payloadLen - offset < 1) return false;
    event.color = pgm_read_byte(command.payload + offset);
    return emitSimple(prefix, source, item, sink, event);
  }
  if (command.type == UiScene::kNodeBitmap) {
    if (!readRect(command, offset, &event.rect, dx, dy) ||
        !readString(command, offset + 8, &event.text, &offset)) return false;
    return emitSimple(prefix, source, item, sink, event);
  }
  if (command.type == UiScene::kNodeLine) {
    uint16_t x = 0, y = 0, x2 = 0, y2 = 0;
    if (!readU16At(command, offset, &x) || !readU16At(command, offset + 2, &y) ||
        !readU16At(command, offset + 4, &x2) ||
        !readU16At(command, offset + 6, &y2) ||
        command.payloadLen - offset < 10) return false;
    event.rect = shifted(
        Rect{static_cast<int16_t>(x), static_cast<int16_t>(y),
              static_cast<uint16_t>(x2 >= x ? x2 - x + 1 : x - x2 + 1),
              static_cast<uint16_t>(y2 >= y ? y2 - y + 1 : y - y2 + 1)},
        dx, dy);
    event.x2 = static_cast<int16_t>(x2 + dx);
    event.y2 = static_cast<int16_t>(y2 + dy);
    event.width = pgm_read_byte(command.payload + offset + 8);
    event.color = pgm_read_byte(command.payload + offset + 9);
    return emitSimple(prefix, source, item, sink, event);
  }
  if (command.type == UiScene::kNodeRect ||
      command.type == UiScene::kNodeRoundRect) {
    if (!readRect(command, offset, &event.rect, dx, dy) ||
        command.payloadLen - offset < (command.type == UiScene::kNodeRect ? 10 : 12))
      return false;
    event.radius = command.type == UiScene::kNodeRoundRect
                       ? UiScene::readU16(command.payload + offset + 8)
                       : 0;
    const size_t styleOffset = offset +
        (command.type == UiScene::kNodeRoundRect ? 10 : 8);
    event.width = pgm_read_byte(command.payload + styleOffset);
    event.fill = pgm_read_byte(command.payload + styleOffset + 1);
    return emitSimple(prefix, source, item, sink, event);
  }
  if (command.type == UiScene::kNodeText) {
    if (!readRect(command, offset, &event.rect, dx, dy) ||
        command.payloadLen - offset < 16) return false;
    event.font = pgm_read_byte(command.payload + offset + 8);
    event.style = pgm_read_byte(command.payload + offset + 9);
    event.align = pgm_read_byte(command.payload + offset + 10);
    const uint8_t isBinding = pgm_read_byte(command.payload + offset + 12);
    const BindingId binding = pgm_read_byte(command.payload + offset + 13);
    uint16_t length = 0;
    if (!readU16At(command, offset + 14, &length) ||
        command.payloadLen - offset - 16 < length) return false;
    if (isBinding != 0) {
      ResolvedValue value{};
      if (!source.read(binding, item.valid ? &item : nullptr, &value) ||
          value.kind != ValueKind::Text) return true;
      event.text = value.text;
    } else {
      event.text = TextView::fromProgmem(
          reinterpret_cast<const char*>(command.payload + offset + 16), length);
    }
    return emitSimple(prefix, source, item, sink, event);
  }
  if (command.type == UiScene::kNodeCover) {
    if (!readRect(command, offset, &event.rect, dx, dy) ||
        command.payloadLen - offset < 12) return false;
    event.radius = UiScene::readU16(command.payload + offset + 8);
    const uint8_t hasBinding = pgm_read_byte(command.payload + offset + 10);
    if (hasBinding != 0) {
      if (command.payloadLen - offset < 12) return false;
      ResolvedValue value{};
      const BindingId binding = pgm_read_byte(command.payload + offset + 11);
      event.assetBinding = binding;
      if (!source.read(binding, item.valid ? &item : nullptr, &value)) return true;
      if (value.kind == ValueKind::Text) {
        event.text = value.text;
      } else if (value.kind == ValueKind::Asset) {
        event.hasAsset = true;
        event.assetIndex = value.assetIndex;
      } else {
        return true;
      }
    }
    return emitSimple(prefix, source, item, sink, event);
  }
  if (command.type == UiScene::kNodeProgress ||
      command.type == UiScene::kNodeBattery) {
    const size_t required = command.type == UiScene::kNodeProgress ? 11 : 9;
    if (!readRect(command, offset, &event.rect, dx, dy) ||
        command.payloadLen - offset < required) return false;
    const size_t bindingOffset = offset +
        (command.type == UiScene::kNodeProgress ? 10 : 8);
    ResolvedValue value{};
    if (!source.read(pgm_read_byte(command.payload + bindingOffset),
                     item.valid ? &item : nullptr, &value) ||
        value.kind != ValueKind::Int) return true;
    event.value = value.number;
    if (command.type == UiScene::kNodeProgress)
      event.radius = UiScene::readU16(command.payload + offset + 8);
    return emitSimple(prefix, source, item, sink, event);
  }
  if (command.type == UiScene::kNodeIcon) {
    if (!readRect(command, offset, &event.rect, dx, dy) ||
        !readString(command, offset + 8, &event.text, &offset) ||
        command.payloadLen - offset < 1) return false;
    const uint8_t hasBinding = pgm_read_byte(command.payload + offset++);
    if (hasBinding != 0) {
      if (command.payloadLen - offset < 1) return false;
      ResolvedValue value{};
      const BindingId binding = pgm_read_byte(command.payload + offset);
      event.assetBinding = binding;
      if (!source.read(binding,
                       item.valid ? &item : nullptr, &value)) return true;
      if (value.kind == ValueKind::Text) {
        event.text = value.text;
      } else if (value.kind == ValueKind::Asset) {
        event.hasAsset = true;
        event.assetIndex = value.assetIndex;
      } else {
        return true;
      }
    }
    return emitSimple(prefix, source, item, sink, event);
  }
  return false;
}

struct HitCapture {
  int16_t x = 0;
  int16_t y = 0;
  HitResult* result = nullptr;
};

inline void captureHit(void* user, const RenderEvent& event) {
  HitCapture* capture = static_cast<HitCapture*>(user);
  if (!capture || !capture->result || capture->result->hit ||
      event.action == UiScene::kInvalidActionId ||
      !event.rect.contains(capture->x, capture->y)) return;
  capture->result->hit = true;
  capture->result->action = event.action;
  capture->result->item = event.item;
  capture->result->rect = event.rect;
  capture->result->argument = event.argument;
}

}  // namespace detail

inline bool renderScene(const uint8_t* data, size_t len,
                        const SceneBindingSource& source,
                        const SceneRenderSink& sink) {
  if (!sink.emit) return false;
  bool ok = true;
  const bool parsed = UiScene::forEachCommand(
      data, len, [&](const SceneCommand& command) {
        if (!detail::executeCommand(command, source, SceneItemContext{}, 0, 0,
                                    sink, 0)) {
          ok = false;
        }
        return true;
      });
  return parsed && ok;
}

inline bool hitTestScene(const uint8_t* data, size_t len,
                         const SceneBindingSource& source, int16_t x,
                         int16_t y, HitResult* out) {
  if (!out) return false;
  *out = HitResult{};
  detail::HitCapture capture{x, y, out};
  const SceneRenderSink sink{&capture, &detail::captureHit};
  if (!renderScene(data, len, source, sink)) return false;
  return out->hit;
}

}  // namespace UiSceneRuntime

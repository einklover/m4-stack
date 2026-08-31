#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ui/scene/UiSceneTypes.h"

namespace UiScene {

constexpr std::size_t kUiSceneActionArgumentBytes = 64;

// Numeric action data only. Page callbacks and backend objects stay outside
// this queue's ABI.
struct UiSceneAction {
  ActionId action = kInvalidActionId;
  uint8_t itemIndex = 0xFF;
  uint16_t itemKey = 0;
  uint8_t argumentLength = 0;
  char argument[kUiSceneActionArgumentBytes] = {};

  bool valid() const {
    return action != kInvalidActionId &&
           argumentLength <= kUiSceneActionArgumentBytes;
  }

  TextView argumentView() const {
    return argumentLength == 0 ? TextView{}
                               : TextView::fromRam(argument, argumentLength);
  }
};

static_assert(std::is_trivially_copyable<UiSceneAction>::value,
              "scene actions must be fixed-capacity values");

enum class UiSceneActionDispatchResult : uint8_t {
  Empty,
  Dispatched,
  Rejected,
};

// Single-producer/single-consumer ring. Enqueue/dequeue never waits and has a
// fixed upper bound; callers must not use multiple producers concurrently.
class UiSceneActionQueue final {
 public:
  static constexpr std::size_t kCapacity = 4;

  bool tryEnqueue(const UiSceneAction& action) {
    if (!action.valid()) return false;
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= kCapacity) return false;
    slots_[head % kCapacity] = action;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool tryDequeue(UiSceneAction& out) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;
    out = slots_[tail % kCapacity];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  std::size_t size() const {
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    const uint32_t count = head - tail;
    return count > kCapacity ? kCapacity : static_cast<std::size_t>(count);
  }

 private:
  UiSceneAction slots_[kCapacity] = {};
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
};

using UiSceneActionHandler = bool (*)(void*, const UiSceneAction&);

// Stateless dispatcher so any page can share the same queue/consumer logic.
class UiSceneActionDispatcher final {
 public:
  UiSceneActionDispatchResult dispatchOne(UiSceneActionQueue& queue,
                                          UiSceneActionHandler handler,
                                          void* user) const {
    UiSceneAction action{};
    if (!queue.tryDequeue(action)) return UiSceneActionDispatchResult::Empty;
    if (!handler || !handler(user, action)) {
      return UiSceneActionDispatchResult::Rejected;
    }
    return UiSceneActionDispatchResult::Dispatched;
  }

  std::size_t dispatchAvailable(UiSceneActionQueue& queue,
                                UiSceneActionHandler handler, void* user,
                                std::size_t budget) const {
    std::size_t dispatched = 0;
    std::size_t processed = 0;
    while (processed < budget) {
      const UiSceneActionDispatchResult result = dispatchOne(queue, handler, user);
      if (result == UiSceneActionDispatchResult::Empty) break;
      ++processed;
      if (result == UiSceneActionDispatchResult::Dispatched) ++dispatched;
    }
    return dispatched;
  }
};

}  // namespace UiScene

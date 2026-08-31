#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#ifdef CROSSPOINT_MURPHY_M4
#include <esp_heap_caps.h>
#endif

// Fixed-capacity snapshot exchange for one producer and multiple readers.
// Concurrent producers are not supported. Publishing never waits: it fails
// when both non-current slots are pinned by readers. Readers also never wait;
// they make one bounded pin attempt per slot and return any safe snapshot.
template <typename T>
class UiStateStore {
  static_assert(std::is_trivially_copyable<T>::value,
                "UiStateStore snapshots must be trivially copyable");

 public:
  static constexpr std::size_t kSlotCount = 3;

  class Snapshot {
   public:
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    Snapshot(Snapshot&& other) noexcept
        : store_(other.store_), slot_(other.slot_) {
      other.store_ = nullptr;
    }

    Snapshot& operator=(Snapshot&& other) noexcept {
      if (this != &other) {
        release();
        store_ = other.store_;
        slot_ = other.slot_;
        other.store_ = nullptr;
      }
      return *this;
    }

    ~Snapshot() { release(); }

    bool valid() const { return store_ != nullptr; }

    const T& value() const { return store_->slots_[slot_].value; }

    std::uint64_t generation() const {
      return store_->slots_[slot_].generation;
    }

    void release() {
      if (store_ != nullptr) {
        store_->readers_[slot_].fetch_sub(1, std::memory_order_release);
        store_ = nullptr;
      }
    }

   private:
    friend class UiStateStore;

    Snapshot() : store_(nullptr), slot_(0) {}

    Snapshot(UiStateStore* store, std::uint8_t slot)
        : store_(store), slot_(slot) {}

    UiStateStore* store_;
    std::uint8_t slot_;
  };

  explicit UiStateStore(const T& initial)
      : slots_(nullptr),
        heapAllocated_(false),
        readers_{},
        accessState_{},
        current_(0),
        generation_(0) {
    // Fixed-capacity store: allocate slots on heap/PSRAM to keep internal RAM free.
    // For small snapshots this is still heap but tiny; for large HomeScenePublication
    // the 3*8148 arenas move to PSRAM (saving ~24KB internal) while metadata
    // (readers/current/generation) stays internal.
#ifdef CROSSPOINT_MURPHY_M4
    Slot* p = static_cast<Slot*>(heap_caps_malloc(sizeof(Slot) * kSlotCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) p = static_cast<Slot*>(heap_caps_malloc(sizeof(Slot) * kSlotCount, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    if (!p) p = static_cast<Slot*>(malloc(sizeof(Slot) * kSlotCount));
#else
    Slot* p = static_cast<Slot*>(malloc(sizeof(Slot) * kSlotCount));
#endif
    if (p) {
      for (size_t i = 0; i < kSlotCount; ++i) {
        p[i].value = initial;
        p[i].generation = 0;
      }
      slots_ = p;
      heapAllocated_ = true;
    } else {
      // Fallback: allocate via new (should not happen in practice; ensures valid store)
      slots_ = static_cast<Slot*>(malloc(sizeof(Slot) * kSlotCount));
      if (slots_) {
        for (size_t i = 0; i < kSlotCount; ++i) {
          slots_[i].value = initial;
          slots_[i].generation = 0;
        }
        heapAllocated_ = true;
      }
    }
  }

  ~UiStateStore() {
    if (slots_) {
#ifdef CROSSPOINT_MURPHY_M4
      heap_caps_free(slots_);
#else
      free(slots_);
#endif
    }
  }

  UiStateStore(const UiStateStore&) = delete;
  UiStateStore& operator=(const UiStateStore&) = delete;

  // For testing/storage contract: whether slots are heap/PSRAM allocated.
  bool isHeapAllocated() const { return heapAllocated_; }
  size_t heapSlotsBytes() const { return heapAllocated_ ? sizeof(Slot) * kSlotCount : 0; }

  Snapshot acquire() {
    if (!slots_) return Snapshot();
    // The writer gate is metadata only and never makes a reader wait for the
    // writer; the publication copy stays in a reserved, non-current slot.
    accessState_.fetch_add(1, std::memory_order_acquire);
    const std::uint8_t preferred = current_.load(std::memory_order_acquire);

    // A reader entrant prevents the writer from changing current_ until this
    // pin is complete. The fixed scan is a defensive fallback for a violated
    // producer contract; in the normal path the first attempt succeeds.
    for (std::size_t offset = 0; offset < kSlotCount; ++offset) {
      const std::uint8_t slot = static_cast<std::uint8_t>(
          (preferred + offset) % kSlotCount);
      const std::uint32_t previous =
          readers_[slot].fetch_add(1, std::memory_order_acquire);
      if ((previous & kWriterReserved) == 0) {
        accessState_.fetch_sub(1, std::memory_order_release);
        return Snapshot(this, slot);
      }
      readers_[slot].fetch_sub(1, std::memory_order_release);
    }

    accessState_.fetch_sub(1, std::memory_order_release);
    // This is reachable only if the single-writer contract is violated.
    return Snapshot();
  }

  // Called by the single producer only; concurrent publication calls are
  // rejected rather than serialized by the store.
  bool tryPublish(const T& next) {
    if (!slots_) return false;
    std::uint32_t expectedAccess = 0;
    if (!accessState_.compare_exchange_strong(
            expectedAccess, kWriterReserved, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      return false;
    }

    const std::uint8_t current = current_.load(std::memory_order_acquire);

    for (std::uint8_t slot = 0; slot < kSlotCount; ++slot) {
      if (slot == current) {
        continue;
      }

      std::uint32_t expected = 0;
      if (!readers_[slot].compare_exchange_strong(
              expected, kWriterReserved, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        continue;
      }

      slots_[slot].value = next;
      slots_[slot].generation = ++generation_;
      // Keep any reader increments that raced with the reservation. Those
      // readers observed the reservation and will promptly subtract them.
      readers_[slot].fetch_and(~kWriterReserved, std::memory_order_release);
      current_.store(slot, std::memory_order_release);
      accessState_.fetch_and(~kWriterReserved, std::memory_order_release);
      return true;
    }

    accessState_.fetch_and(~kWriterReserved, std::memory_order_release);
    return false;
  }

 private:
  static constexpr std::uint32_t kWriterReserved =
      static_cast<std::uint32_t>(1u) << 31;

  struct Slot {
    T value;
    std::uint64_t generation;
  };

  Slot* slots_;
  bool heapAllocated_ = false;
  std::atomic<std::uint32_t> readers_[kSlotCount];
  std::atomic<std::uint32_t> accessState_;
  std::atomic<std::uint8_t> current_;
  std::uint64_t generation_;
};

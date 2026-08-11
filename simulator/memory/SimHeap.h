// SimHeap: ESP-IDF-style capability heap with REAL first-fit allocation over
// per-region byte pools. Fragmentation is real (alloc/free interleaving leaves
// real holes), so "total free OK but largest block too small" genuinely OOMs —
// the historical bug class on Murphy M4.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/SimKernel.h"

namespace m4sim {

// Capability bits mirroring ESP32 heap_caps.
constexpr uint32_t MALLOC_CAP_8BIT = 1 << 0;
constexpr uint32_t MALLOC_CAP_INTERNAL = 1 << 1;
constexpr uint32_t MALLOC_CAP_SPIRAM = 1 << 2;
constexpr uint32_t MALLOC_CAP_DMA = 1 << 3;

// Memory contract: which allocations MUST live in PSRAM (framebuffer, TTF cmap,
// GBK decode window, page read buffers). If a tagged alloc lands in internal
// RAM, that is a CONTRACT_VIOLATION regardless of whether memory is "available".
struct MemoryContract {
  std::string tag;     // e.g. "framebuffer"
  uint32_t minCaps;    // region must provide at least these caps
  uint32_t maxBytes;   // 0 = any size (enforced on alloc)
};

// One contiguous heap region with its own first-fit allocator (like multi_heap).
// Blocks are doubly-linked in address order; free blocks additionally form a
// singly-linked free list. Coalescing checks address neighbors directly.
// freeBytes_/largestBlock_ are recomputed from the free list after every
// mutation (never maintained incrementally — that was the source of the
// accounting drift).
class SimHeapRegion {
public:
  SimHeapRegion(const std::string& name, uint32_t caps, size_t bytes)
      : name_(name), caps_(caps), mem_(bytes, 0) {
    // Single free block spanning everything after the first header.
    header(0)->size = bytes - kHeader;
    header(0)->isFree = 1;
    header(0)->prev = kNo;
    header(0)->next = kNo;
    header(0)->freeNext = kNo;
    head_ = 0;
    freeBytes_ = bytes - kHeader;
    largestBlock_ = bytes - kHeader;
  }

  uint32_t caps() const { return caps_; }
  const std::string& name() const { return name_; }
  size_t freeBytes() const { return freeBytes_; }
  size_t largestBlock() const { return largestBlock_; }
  const uint8_t* dataPtr() const { return mem_.data(); }
  size_t capacity() const { return mem_.size(); }

  // First-fit allocation inside this region. Returns data pointer or nullptr.
  void* alloc(size_t size) {
    if (size == 0) size = 1;
    size_t off = head_;
    while (off != kNo) {
      Block* b = header(off);
      if (b->isFree && b->size >= size) {
        return splitAlloc(off, b, size);
      }
      off = b->freeNext;
    }
    return nullptr;
  }

  // Returns true if the block was allocated and freed; false on double-free
  // (the caller decides policy — the simulator asserts, real ESP aborts).
  bool free(void* ptr) {
    if (ptr == nullptr) return false;
    size_t off = blockOf(ptr);
    Block* b = header(off);
    if (b->isFree) return false;  // double free — report, don't corrupt the list
    b->isFree = 1;

    // Coalesce with the NEXT block if free.
    if (b->next != kNo) {
      Block* nb = header(b->next);
      if (nb->isFree) {
        removeFromFreeList(b->next);
        b->size += kHeader + nb->size;
        b->next = nb->next;
        if (nb->next != kNo) header(nb->next)->prev = off;
      }
    }
    // Coalesce with the PREVIOUS block if free: fold `b` into `pb`.
    if (b->prev != kNo) {
      Block* pb = header(b->prev);
      if (pb->isFree) {
        removeFromFreeList(b->prev);
        pb->size += kHeader + b->size;
        pb->next = b->next;
        if (b->next != kNo) header(b->next)->prev = b->prev;  // next now points at merged block
        b->isFree = 0;  // folded block is no longer a distinct block
        off = b->prev;
        b = pb;
      }
    }
    pushFree(off, b);
    recomputeStats();
    return true;
  }

  // Baseline: record reserved static usage so the simulated "after boot" heap
  // matches a real device (small internal free + modest largest block).
  // Chunks arrive as interleaved keep/hole pairs: kept blocks (static/bss/task
  // stacks) are pinned forever; hole blocks are allocated BETWEEN them and only
  // freed after every block is placed, so the holes end up sandwiched between
  // kept blocks — that is what actually fragments the region. (Freeing a hole
  // immediately after allocating it would coalesce it back into the tail and
  // leave a single huge free block — no fragmentation at all.)
  // Allocations that don't fit are skipped (caller decides region sizing).
  void reserveBaseline(const std::vector<size_t>& chunks) {
    std::vector<void*> holes;
    for (size_t i = 0; i < chunks.size(); ++i) {
      void* p = alloc(chunks[i]);
      if (!p) continue;  // region too small for this chunk — skip
      if (i % 2 == 0) {
        reserved_.push_back(p);  // keep allocated (static/bss/tasks)
      } else {
        holes.push_back(p);      // hold, free only after all blocks placed
      }
    }
    for (void* h : holes) free(h);
  }

  // Grow in place when the block's free successor makes it possible. Returns
  // the same pointer, or nullptr (caller falls back to alloc+copy).
  void* realloc(void* ptr, size_t size) {
    if (ptr == nullptr) return alloc(size);
    if (size == 0) {
      free(ptr);
      return nullptr;
    }
    size_t off = blockOf(ptr);
    Block* b = header(off);
    if (b->isFree) return nullptr;  // not a live block
    if (b->size >= size) return ptr;  // shrink in place
    size_t grow = size - b->size;
    if (b->next != kNo) {
      Block* nb = header(b->next);
      if (nb->isFree && nb->size + kHeader >= grow) {
        // Absorb enough of the successor to grow in place.
        removeFromFreeList(b->next);
        if (nb->size >= grow + kHeader) {
          // Enough room for a new rest header: b consumes the successor's
          // header plus `grow` bytes of its payload (growth = kHeader + grow).
          // Layout after split: [b][nb hdr + grow payload][rest: free block]
          size_t restOff = b->next + kHeader + grow;
          Block* rest = header(restOff);
          rest->size = nb->size - grow - kHeader;  // succ payload minus consumed minus new hdr
          rest->isFree = 1;
          rest->prev = off;
          rest->next = nb->next;
          if (nb->next != kNo) header(nb->next)->prev = restOff;
          b->size += kHeader + grow;  // b's payload end == restOff (contiguous)
          b->next = restOff;
          pushFree(restOff, rest);
        } else {
          // Not enough for a rest header: consume the whole successor
          // (payload + its header), which covers grow <= nb->size + kHeader.
          b->size += nb->size + kHeader;
          b->next = nb->next;
          if (nb->next != kNo) header(nb->next)->prev = off;
        }
        recomputeStats();
        return ptr;
      }
    }
    return nullptr;  // cannot grow in place
  }

  // Self-check: full allocator invariants. Returns empty string when
  // consistent, else a description of the FIRST violation found.
  // Invariants checked:
  //   1. address chain: no cycle, prev/next symmetric, no gaps, no overlap,
  //      block coverage == region capacity (last block ends at region end)
  //   2. next block header is EXACTLY at off + kHeader + payload (contiguity)
  //   3. free list: no cycle, no duplicates, every node marked free
  //   4. free-block membership: exactly the free address-list blocks are in
  //      the free list (no allocated block leaks into it, no free block lost)
  //   5. freeBytes_ == sum of free payloads, largestBlock_ == max
  std::string checkConsistency() const {
    std::vector<size_t> freeInAddressOrder;
    size_t off = 0;
    size_t steps = 0;
    size_t covered = 0;
    size_t freeCount = 0;
    while (off != kNo) {
      Block* b = header(off);
      if (off != 0 && b->prev != prevBlockOf(off))
        return "prev link broken at offset " + std::to_string(off);
      // Contiguity: the next block header must sit exactly at payload end.
      size_t nextOff = off + kHeader + b->size;
      if (b->next != kNo && b->next != nextOff)
        return "non-contiguous chain: block " + std::to_string(off) + " payload end " +
               std::to_string(nextOff) + " != next " + std::to_string(b->next);
      if (b->next == kNo && nextOff != mem_.size())
        return "last block does not reach region end (gap of " +
               std::to_string(mem_.size() - nextOff) + " bytes)";
      if (nextOff > mem_.size())
        return "block overflow at offset " + std::to_string(off);
      if (b->next != kNo && header(b->next)->prev != off)
        return "next->prev not symmetric at " + std::to_string(b->next);
      covered += kHeader + b->size;
      if (b->isFree) {
        freeCount++;
        freeInAddressOrder.push_back(off);
      }
      if (++steps > mem_.size() / (kHeader + 8) + 1) return "address list cycle";
      off = b->next;
    }
    if (covered != mem_.size())
      return "block coverage " + std::to_string(covered) + " != region capacity " +
             std::to_string(mem_.size());

    // Free list: no cycle, no duplicates, every node marked free.
    std::vector<size_t> freeListNodes;
    size_t seen = 0;
    size_t cur = head_;
    while (cur != kNo) {
      if (!header(cur)->isFree)
        return "free list contains an allocated block at " + std::to_string(cur);
      for (size_t f : freeListNodes)
        if (f == cur) return "free list duplicate at " + std::to_string(cur);
      freeListNodes.push_back(cur);
      if (++seen > mem_.size() / (kHeader + 8) + 1) return "free list cycle detected";
      cur = header(cur)->freeNext;
    }
    // Membership: exactly the address-order free blocks are in the free list.
    if (freeListNodes.size() != freeCount)
      return "free list count " + std::to_string(freeListNodes.size()) + " != address free count " +
             std::to_string(freeCount);
    for (size_t f : freeInAddressOrder) {
      bool found = false;
      for (size_t n : freeListNodes)
        if (n == f) { found = true; break; }
      if (!found) return "free block " + std::to_string(f) + " missing from free list";
    }
    // Stats must match reality.
    size_t sum = 0;
    size_t max = 0;
    for (size_t n : freeListNodes) {
      sum += header(n)->size;
      if (header(n)->size > max) max = header(n)->size;
    }
    if (sum != freeBytes_)
      return "freeBytes_=" + std::to_string(freeBytes_) + " != actual sum " + std::to_string(sum);
    if (max != largestBlock_)
      return "largestBlock_=" + std::to_string(largestBlock_) + " != actual max " +
             std::to_string(max);
    return "";
  }

private:
  static constexpr uint32_t kHeader = 32;
  static constexpr uint32_t kNo = 0xFFFFFFFFu;

#pragma pack(push, 1)
  struct Block {
    uint32_t size;     // payload bytes (excluding header)
    uint32_t isFree;   // 0 used, 1 free
    uint32_t prev;     // address-order prev block offset (kNo for first)
    uint32_t next;     // address-order next block offset (kNo for last)
    uint32_t freeNext; // next free block offset (free list)
  };
#pragma pack(pop)

  Block* header(size_t off) const {
    return reinterpret_cast<Block*>(const_cast<uint8_t*>(mem_.data() + off));
  }
  size_t blockOf(void* ptr) const { return (uint8_t*)ptr - mem_.data() - kHeader; }
  size_t prevBlockOf(size_t off) const {
    size_t o = 0;
    while (o != kNo && header(o)->next != off) o = header(o)->next;
    return o;
  }

  void* splitAlloc(size_t off, Block* b, size_t size) {
    size_t remain = b->size - size;
    if (remain >= kHeader + 16) {
      // Tail becomes a new free block, spliced into the free list in place of b.
      size_t tailOff = off + kHeader + size;
      Block* tail = header(tailOff);
      tail->size = remain - kHeader;
      tail->isFree = 1;
      tail->prev = off;
      tail->next = b->next;
      tail->freeNext = b->freeNext;
      if (b->next != kNo) header(b->next)->prev = tailOff;
      // Allocated block is the head; its next is now the new tail block.
      b->size = size;
      b->isFree = 0;
      b->next = tailOff;
      b->freeNext = kNo;
      replaceInFreeList(off, tailOff);
      recomputeStats();
      return mem_.data() + off + kHeader;
    }
    // Whole block.
    b->isFree = 0;
    removeFromFreeList(off);
    recomputeStats();
    return mem_.data() + off + kHeader;
  }

  void pushFree(size_t off, Block* b) {
    b->freeNext = head_;
    head_ = off;
  }

  // Find the free-list node whose freeNext == target and point it at
  // `replacement` (used when a split replaces a block in place).
  void replaceInFreeList(size_t target, size_t replacement) {
    if (head_ == target) {
      head_ = replacement;
      return;
    }
    size_t cur = head_;
    while (cur != kNo && header(cur)->freeNext != target) cur = header(cur)->freeNext;
    if (cur != kNo) header(cur)->freeNext = replacement;  // target must be in list
  }

  // Remove `off` from the free list.
  void removeFromFreeList(size_t off) {
    if (head_ == off) {
      head_ = header(off)->freeNext;
      header(off)->freeNext = kNo;
      return;
    }
    size_t cur = head_;
    while (cur != kNo && header(cur)->freeNext != off) cur = header(cur)->freeNext;
    if (cur != kNo) header(cur)->freeNext = header(off)->freeNext;
    header(off)->freeNext = kNo;
  }

  // Recompute freeBytes_ and largestBlock_ from the free list. Called after
  // every mutation — incremental bookkeeping is what drifted before.
  void recomputeStats() {
    freeBytes_ = 0;
    largestBlock_ = 0;
    size_t cur = head_;
    while (cur != kNo) {
      Block* b = header(cur);
      freeBytes_ += b->size;
      if (b->size > largestBlock_) largestBlock_ = b->size;
      cur = b->freeNext;
    }
  }

  std::string name_;
  uint32_t caps_;
  std::vector<uint8_t> mem_;
  size_t head_ = kNo;      // free list head
  size_t freeBytes_ = 0;
  size_t largestBlock_ = 0;
  std::vector<void*> reserved_;
};

// Top-level heap: routes allocations to regions by ESP-IDF capability
// semantics, enforces memory contracts, records an allocation trace, and
// reports OOM with the live-alloc snapshot.
//
// Capability semantics match ESP-IDF: a region is eligible iff
//   (region.caps & requestedCaps) == requestedCaps
// i.e. the region must satisfy the WHOLE requested capability set — NOT
// "any of". MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT requires an internal 8-bit
// region, never "internal OR 8bit".
//
// Multiple independent INTERNAL regions model the real SoC's separate DRAM
// banks: free blocks in different regions can NEVER coalesce (A's 20K tail +
// B's 30K head ≠ one 50K block). This is the difference between "largest free
// block" and "sum of free bytes" — the OOM-class bug detector.
class SimHeap {
public:
  struct LiveAlloc {
    size_t size;
    uint32_t caps;
    std::string heap;
    std::string tag;
    int seq;
    void* ptr;
  };

  SimHeap(SimTrace* trace, std::function<uint32_t()> now = []() { return 0u; })
      : trace_(trace), now_(std::move(now)) {
    // Internal RAM split into separate banks (like the S3's DRAM regions),
    // each with its own capability set. Free space in one bank can never
    // satisfy an allocation that needs a contiguous block spanning banks.
    regions_.push_back(std::make_unique<SimHeapRegion>(
        "INTERNAL_0", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, kInternal0Bytes));
    regions_.push_back(std::make_unique<SimHeapRegion>(
        "INTERNAL_1", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA,
        kInternal1Bytes));
    regions_.push_back(std::make_unique<SimHeapRegion>(
        "INTERNAL_2", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, kInternal2Bytes));
    regions_.push_back(std::make_unique<SimHeapRegion>(
        "PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, kPsramBytes));
  }

  void setNow(std::function<uint32_t()> now) { now_ = std::move(now); }

  static constexpr size_t kInternal0Bytes = 256 * 1024;
  static constexpr size_t kInternal1Bytes = 128 * 1024;
  static constexpr size_t kInternal2Bytes = 128 * 1024;
  static constexpr size_t kPsramBytes = 8 * 1024 * 1024;

  SimHeapRegion& internal() { return *regions_[0]; }
  SimHeapRegion& psram() { return *regions_[3]; }
  const std::vector<std::unique_ptr<SimHeapRegion>>& regions() const { return regions_; }

  void expect(const MemoryContract& c) { contracts_[c.tag] = c; }

  // Allocate honoring ESP-IDF caps semantics. Tag is optional (drives contract
  // + trace).
  void* alloc(size_t size, uint32_t caps, const char* tag = nullptr) {
    if (size == 0) size = 1;
    allocCount_++;
    if (failNth_ > 0 && allocCount_ == failNth_) {
      oomReport(size, caps, tag, "fault inject fail-Nth");
      return nullptr;
    }
    if (failInternalOnly_ && (caps & MALLOC_CAP_INTERNAL)) {
      oomReport(size, caps, tag, "fault inject internal-only");
      return nullptr;
    }

    // Eligible = regions whose cap set is a SUPERSET of the request.
    std::vector<SimHeapRegion*> eligible;
    for (auto& r : regions_)
      if ((r->caps() & caps) == caps) eligible.push_back(r.get());

    // Preference order (roughly ESP policy): for large multi-cap requests try
    // PSRAM first (it's cheap to give big buffers external RAM); otherwise try
    // internal banks first, PSRAM as fallback. First-fit within the order.
    void* p = nullptr;
    SimHeapRegion* region = nullptr;
    bool capsAllowPsram = (caps & MALLOC_CAP_SPIRAM);
    if (size >= 32 * 1024 && capsAllowPsram) {
      for (auto* r : eligible) {
        if (!(r->caps() & MALLOC_CAP_SPIRAM)) continue;
        p = r->alloc(size);
        if (p) { region = r; break; }
      }
    }
    if (!p) {
      for (auto* r : eligible) {
        if ((r->caps() & MALLOC_CAP_SPIRAM) && size >= 32 * 1024) continue;  // tried above
        p = r->alloc(size);
        if (p) { region = r; break; }
      }
    }

    if (!p) {
      oomReport(size, caps, tag, "no region fits");
      return nullptr;
    }

    // Memory contract enforcement.
    auto it = contracts_.find(tag ? tag : "");
    if (it != contracts_.end()) {
      const MemoryContract& c = it->second;
      bool capsOk = (region->caps() & c.minCaps) == c.minCaps;
      bool sizeOk = (c.maxBytes == 0) || (size <= c.maxBytes);
      if (!capsOk || !sizeOk) {
        std::string reason = capsOk ? "size-exceeds-max" : "wrong-region";
        trace_->emit(SimEventType::CONTRACT_VIOLATION,
                     "allocation=" + std::string(tag) + " size=" + std::to_string(size) +
                         " max=" + std::to_string(c.maxBytes) +
                         " expected=" + capsName(c.minCaps) +
                         " actual=" + region->name() + " reason=" + reason,
                      now_());
        contractViolations_++;
        free(p);
        return nullptr;
      }
    }

    liveAllocs_.push_back({size, caps, region->name(), tag ? tag : "", allocCount_, p});
    char buf[160];
    snprintf(buf, sizeof(buf), "size=%zu caps=%s heap=%s free_after=%zu largest_after=%zu tag=%s",
             size, capsName(caps).c_str(), region->name().c_str(), region->freeBytes(),
             region->largestBlock(), tag ? tag : "-");
    trace_->emit(SimEventType::ALLOC, buf, now_());
    return p;
  }

  void free(void* p) {
    if (!p) return;
    // Drop the live record FIRST (so an OOM report after a free is accurate).
    for (size_t i = 0; i < liveAllocs_.size(); ++i) {
      if (liveAllocs_[i].ptr == p) {
        liveAllocs_.erase(liveAllocs_.begin() + i);
        break;
      }
    }
    bool freed = false;
    for (auto& r : regions_) {
      const uint8_t* p8 = (const uint8_t*)p;
      if (p8 >= r->dataPtr() && p8 < r->dataPtr() + r->capacity()) {
        freed = r->free(p);
        break;
      }
    }
    if (!freed) {
      // Double free (or a pointer that never came from this heap): the
      // simulator must surface this like a real allocator abort would.
      trace_->emit(SimEventType::ASSERT,
                   "double free or foreign pointer " + std::to_string((uintptr_t)p), now_());
      doubleFrees_++;
    }
    trace_->emit(SimEventType::FREE, "", now_());
  }

  int doubleFrees() const { return doubleFrees_; }

  void* calloc(size_t n, size_t sz, uint32_t caps, const char* tag = nullptr) {
    // Overflow guard: the real calloc aborts on overflow; the simulator
    // refuses the request (an ALLOC trace would never happen).
    if (n != 0 && sz > (size_t)-1 / n) return nullptr;
    void* p = alloc(n * sz, caps, tag);
    if (p) memset(p, 0, n * sz);
    return p;
  }

  // ESP-style realloc: grow/shrink in place when the region allows AND still
  // satisfies the requested caps; else move to a qualifying region. Memory
  // contracts apply exactly as on alloc (realloc must NOT bypass them).
  // Returns the new pointer (may differ); nullptr only on OOM/contract-violation
  // (old ptr kept alive).
  void* realloc(void* p, size_t size, uint32_t caps, const char* tag = nullptr) {
    if (p == nullptr) return alloc(size, caps, tag);
    if (size == 0) {
      free(p);
      return nullptr;
    }
    // Contract enforcement FIRST — a contract violation must be reported even
    // when in-place growth would succeed (review: realloc must not bypass
    // the ordinary alloc contract).
    auto it = contracts_.find(tag ? tag : "");
    if (it != contracts_.end()) {
      const MemoryContract& c = it->second;
      bool sizeOk = (c.maxBytes == 0) || (size <= c.maxBytes);
      if (!sizeOk) {
        trace_->emit(SimEventType::CONTRACT_VIOLATION,
                     "realloc=" + std::string(tag) + " size=" + std::to_string(size) +
                         " max=" + std::to_string(c.maxBytes) + " reason=size-exceeds-max",
                      now_());
        contractViolations_++;
        return nullptr;  // old block stays alive
      }
    }

    // Find the owning region.
    SimHeapRegion* owner = nullptr;
    for (auto& r : regions_) {
      const uint8_t* p8 = (const uint8_t*)p;
      if (p8 >= r->dataPtr() && p8 < r->dataPtr() + r->capacity()) {
        owner = r.get();
        break;
      }
    }
    if (!owner) return nullptr;  // foreign pointer — treat as no-op

    // In-place is legal ONLY if the owner still satisfies the new caps.
    bool capsSatisfied = (owner->caps() & caps) == caps;
    if (capsSatisfied && size <= blockSizeOf(owner, p)) {
      // Shrink in place.
      void* q = owner->realloc(p, size);
      if (q) {
        updateLiveSize(p, q, size);
        return q;
      }
    }
    if (capsSatisfied) {
      void* q = owner->realloc(p, size);  // try grow in place
      if (q) {
        updateLiveSize(p, q, size);
        return q;
      }
    }
    // Cannot (or must not) grow in place: move to a qualifying region.
    void* q = alloc(size, caps, tag);
    if (!q) return nullptr;  // OOM: keep old block alive (like ESP realloc)
    size_t oldSize = 0;
    for (auto& a : liveAllocs_)
      if (a.ptr == p) { oldSize = a.size; break; }
    memcpy(q, p, oldSize < size ? oldSize : size);
    free(p);
    return q;
  }

  void updateLiveSize(void* oldPtr, void* newPtr, size_t size) {
    for (auto& a : liveAllocs_)
      if (a.ptr == oldPtr) { a.ptr = newPtr; a.size = size; break; }
  }

  size_t blockSizeOf(SimHeapRegion* r, void* p) const {
    (void)r;
    for (auto& a : liveAllocs_)
      if (a.ptr == p) return a.size;
    return 0;
  }

  // Sum of free bytes across all internal banks. NOTE: this is NOT the largest
  // allocatable block — separate banks never coalesce, so largestInternal()
  // is the max over banks, which can be far smaller than the sum. That gap is
  // exactly the fragmentation OOM class.
  size_t freeInternal() const {
    size_t sum = 0;
    for (size_t i = 0; i < regions_.size() - 1; ++i) sum += regions_[i]->freeBytes();
    return sum;
  }
  size_t largestInternal() const {
    size_t max = 0;
    for (size_t i = 0; i < regions_.size() - 1; ++i) {
      size_t lb = regions_[i]->largestBlock();
      if (lb > max) max = lb;
    }
    return max;
  }
  size_t freePsram() const { return regions_.back()->freeBytes(); }
  size_t largestPsram() const { return regions_.back()->largestBlock(); }

  void setFailNth(int n) { failNth_ = n; }
  void setFailInternalOnly(bool b) { failInternalOnly_ = b; }
  int contractViolations() const { return contractViolations_; }

  // Apply a boot baseline: fragment EVERY internal bank so the free/largest
  // numbers look like a real booted device. Each bank gets the same pattern
  // (banks are independent — a TLS reserve must fail in EVERY bank for OOM).
  void applyBootBaseline(const std::vector<size_t>& chunks) {
    for (size_t i = 0; i < regions_.size() - 1; ++i) regions_[i]->reserveBaseline(chunks);
  }

  std::string oomReport(size_t size, uint32_t caps, const char* /*tag*/, const char* why) {
    char buf[512];
    std::string bankInfo;
    for (size_t i = 0; i < regions_.size() - 1; ++i) {
      char b[96];
      snprintf(b, sizeof(b), " %s:free=%zu/largest=%zu", regions_[i]->name().c_str(),
               regions_[i]->freeBytes(), regions_[i]->largestBlock());
      bankInfo += b;
    }
    snprintf(buf, sizeof(buf),
             "requested=%zu caps=%s internal_free=%zu internal_largest=%zu psram_free=%zu "
             "psram_largest=%zu why=%s banks=%s",
             size, capsName(caps).c_str(), freeInternal(), largestInternal(), freePsram(),
             largestPsram(), why, bankInfo.c_str());
    std::string msg = buf;
    // Rank live allocations by size — the "top live allocations" the OOM report
    // should surface (e.g. "framebuffer is eligible for PSRAM").
    std::vector<LiveAlloc> sorted = liveAllocs_;
    std::sort(sorted.begin(), sorted.end(),
              [](const LiveAlloc& a, const LiveAlloc& b) { return a.size > b.size; });
    for (auto& a : sorted)
      msg += "\n  live: tag=" + a.tag + " size=" + std::to_string(a.size) + " heap=" + a.heap;
    trace_->emit(SimEventType::OOM, msg, now_());
    return msg;
  }

  // Diagnostics for the heap stress self-test.
  std::string checkConsistency() const {
    std::string out;
    for (auto& r : regions_) out += r->checkConsistency();
    return out;
  }
  const std::vector<LiveAlloc>& liveAllocs() const { return liveAllocs_; }

private:
  static std::string capsName(uint32_t caps) {
    std::string s;
    if (caps & MALLOC_CAP_INTERNAL) s += "INTERNAL|";
    if (caps & MALLOC_CAP_SPIRAM) s += "PSRAM|";
    if (caps & MALLOC_CAP_8BIT) s += "8BIT|";
    if (caps & MALLOC_CAP_DMA) s += "DMA|";
    if (!s.empty()) s.pop_back();
    return s;
  }

  SimTrace* trace_;
  std::function<uint32_t()> now_;
  std::vector<std::unique_ptr<SimHeapRegion>> regions_;
  std::map<std::string, MemoryContract> contracts_;
  std::vector<LiveAlloc> liveAllocs_;
  int allocCount_ = 0;
  int failNth_ = 0;
  bool failInternalOnly_ = false;
  int contractViolations_ = 0;
  int doubleFrees_ = 0;
};

}  // namespace m4sim

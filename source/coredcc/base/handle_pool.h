#pragma once

#include "coredcc/base/assert.h"

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <compare>
#include <cstdint>
#include <new>
#include <utility>

namespace coredcc {

/**
 * A generational handle into a HandlePool<T>.
 *
 * Generations start at 1, so the default `{0, 0}` handle is null and never valid. A
 * handle stays invalid forever once its object is destroyed: a reused slot gets a new
 * generation, and a slot whose generation would wrap is retired instead.
 */
template <typename T> struct Handle {
  uint32_t index = 0;
  uint32_t generation = 0;

  constexpr bool isNull() const
  {
    return generation == 0;
  }
  constexpr explicit operator bool() const
  {
    return generation != 0;
  }
  constexpr auto operator<=>(const Handle &) const = default;
  uint64_t computeHash() const
  {
    return (uint64_t(generation) << 32) | index;
  }
};

/**
 * Owns objects of type T at stable addresses and hands out generational handles.
 *
 * Storage is a list of fixed-size pages, so pointers stay valid until the object is
 * destroyed. Quarantine mode never reuses a slot, so a stale handle in a test always
 * resolves to null instead of to an unrelated new object.
 */
template <typename T, int PageSize = 256> class HandlePool {
  static_assert(PageSize > 0 && (PageSize & (PageSize - 1)) == 0, "PageSize must be a power of 2");

 public:
  using HandleT = Handle<T>;

  HandlePool() = default;
  HandlePool(const HandlePool &) = delete;
  HandlePool &operator=(const HandlePool &) = delete;
  ~HandlePool()
  {
    clear();
    for (Page *page : pages_) {
      litestl::alloc::Delete(page);
    }
  }

  void setQuarantine(bool on)
  {
    quarantine_ = on;
  }
  bool quarantine() const
  {
    return quarantine_;
  }

  template <typename... Args> HandleT create(Args &&...args)
  {
    uint32_t index;
    if (!quarantine_ && free_.size() > 0) {
      index = free_.pop_back();
    }
    else {
      index = slotCount_++;
      if ((index & (PageSize - 1)) == 0) {
        pages_.append(litestl::alloc::New<Page>("coredcc::HandlePool page"));
      }
    }
    Slot &slot = slotAt(index);
    if (slot.generation == 0) {
      slot.generation = 1;
    }
    new (slot.storage) T(std::forward<Args>(args)...);
    slot.live = true;
    live_++;
    return HandleT{index, slot.generation};
  }

  /** Destroy the object. Returns false if the handle was already invalid. */
  bool destroy(HandleT h)
  {
    Slot *slot = liveSlot(h);
    if (!slot) {
      return false;
    }
    slot->object()->~T();
    slot->live = false;
    live_--;
    if (slot->generation == UINT32_MAX) {
      return true; // retired: never reused, so no handle can alias a later object
    }
    slot->generation++;
    if (!quarantine_) {
      free_.append(h.index);
    }
    return true;
  }

  T *get(HandleT h)
  {
    Slot *slot = liveSlot(h);
    return slot ? slot->object() : nullptr;
  }
  const T *get(HandleT h) const
  {
    return const_cast<HandlePool *>(this)->get(h);
  }
  bool valid(HandleT h) const
  {
    return get(h) != nullptr;
  }

  /** Number of live objects. */
  int size() const
  {
    return live_;
  }

  /** Destroy every live object. Outstanding handles become invalid. */
  void clear()
  {
    for (uint32_t i = 0; i < slotCount_; i++) {
      Slot &slot = slotAt(i);
      if (slot.live) {
        destroy(HandleT{i, slot.generation});
      }
    }
  }

  /** Call fn(HandleT, T&) for every live object, in index order. */
  template <typename Fn> void forEach(Fn &&fn)
  {
    for (uint32_t i = 0; i < slotCount_; i++) {
      Slot &slot = slotAt(i);
      if (slot.live) {
        fn(HandleT{i, slot.generation}, *slot.object());
      }
    }
  }

  /** Test hook: force a slot's generation, to exercise retirement. */
  void debugSetGeneration(uint32_t index, uint32_t generation)
  {
    CDCC_CHECK(index < slotCount_ && !slotAt(index).live, "slot must exist and be free");
    slotAt(index).generation = generation;
  }

 private:
  struct Slot {
    alignas(T) unsigned char storage[sizeof(T)];
    uint32_t generation = 0;
    bool live = false;

    T *object()
    {
      return std::launder(reinterpret_cast<T *>(storage));
    }
  };
  struct Page {
    Slot slots[PageSize];
  };

  Slot &slotAt(uint32_t index)
  {
    return pages_[index / PageSize]->slots[index & (PageSize - 1)];
  }
  Slot *liveSlot(HandleT h)
  {
    if (h.generation == 0 || h.index >= slotCount_) {
      return nullptr;
    }
    Slot &slot = slotAt(h.index);
    return slot.live && slot.generation == h.generation ? &slot : nullptr;
  }

  litestl::util::Vector<Page *> pages_;
  litestl::util::Vector<uint32_t> free_;
  uint32_t slotCount_ = 0;
  int live_ = 0;
  bool quarantine_ = false;
};

} // namespace coredcc

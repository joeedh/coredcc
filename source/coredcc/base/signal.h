#pragma once

#include "coredcc/base/assert.h"

#include "litestl/util/function.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace coredcc {

namespace detail {
struct SignalStateBase {
  virtual ~SignalStateBase() = default;
  virtual void disconnect(uint64_t id) = 0;
  virtual bool connected(uint64_t id) const = 0;
};
} // namespace detail

/**
 * A handle to one connection. Copyable; dropping it does NOT disconnect (use
 * ScopedConnection for that). It holds a weak reference, so it may outlive its Signal:
 * disconnecting then does nothing.
 */
class Connection {
public:
  Connection() = default;
  Connection(std::weak_ptr<detail::SignalStateBase> state, uint64_t id)
      : state_(std::move(state)), id_(id)
  {
  }

  void disconnect()
  {
    if (auto s = state_.lock()) {
      s->disconnect(id_);
    }
    state_.reset();
  }
  bool connected() const
  {
    auto s = state_.lock();
    return s && s->connected(id_);
  }

private:
  std::weak_ptr<detail::SignalStateBase> state_;
  uint64_t id_ = 0;
};

/** Disconnects on destruction. Move-only. */
class ScopedConnection {
public:
  ScopedConnection() = default;
  ScopedConnection(Connection c) : c_(std::move(c))
  {
  }
  ScopedConnection(ScopedConnection &&b) noexcept : c_(std::exchange(b.c_, Connection()))
  {
  }
  ScopedConnection &operator=(ScopedConnection &&b) noexcept
  {
    if (this != &b) {
      c_.disconnect();
      c_ = std::exchange(b.c_, Connection());
    }
    return *this;
  }
  ScopedConnection(const ScopedConnection &) = delete;
  ScopedConnection &operator=(const ScopedConnection &) = delete;
  ~ScopedConnection()
  {
    c_.disconnect();
  }

  void disconnect()
  {
    c_.disconnect();
  }
  bool connected() const
  {
    return c_.connected();
  }
  Connection release()
  {
    return std::exchange(c_, Connection());
  }

private:
  Connection c_;
};

/**
 * Multicast callbacks, safe against re-entrancy. The contract:
 * - A slot connected during an emit is not called by that emit.
 * - A slot disconnected during an emit (by itself or another slot) is not called again
 *   by that emit.
 * - Emitting from inside a slot (re-entrant emit) is allowed.
 * - Destroying a Signal while it is emitting is a bug, caught in debug builds.
 *
 * Single-threaded, like the rest of coredcc's public API.
 */
template <typename... Args> class Signal {
public:
  using Fn = litestl::util::function<void(Args...)>;

  Signal() : state_(std::make_shared<State>())
  {
  }
  Signal(const Signal &) = delete;
  Signal &operator=(const Signal &) = delete;
  ~Signal()
  {
    CDCC_ASSERT(state_->emitDepth == 0, "Signal destroyed while emitting");
  }

  Connection connect(Fn fn)
  {
    uint64_t id = state_->nextId++;
    state_->slots.append(Slot{id, std::make_shared<Fn>(std::move(fn)), true});
    return Connection(std::weak_ptr<detail::SignalStateBase>(state_), id);
  }

  void emit(Args... args)
  {
    std::shared_ptr<State> keep = state_; // a buggy slot that destroys us won't free this
    State &s = *keep;
    s.emitDepth++;
    const int n = int(s.slots.size()); // later connects aren't called by this emit
    for (int i = 0; i < n; i++) {
      if (!s.slots[i].live) {
        continue;
      }
      std::shared_ptr<Fn> fn = s.slots[i].fn; // the slots vector may grow under us
      (*fn)(args...);
    }
    if (--s.emitDepth == 0 && s.needsCompact) {
      s.compact();
    }
  }

  int slotCount() const
  {
    int n = 0;
    for (const Slot &slot : state_->slots) {
      n += slot.live ? 1 : 0;
    }
    return n;
  }

  void disconnectAll()
  {
    for (Slot &slot : state_->slots) {
      slot.live = false;
    }
    state_->needsCompact = true;
    if (state_->emitDepth == 0) {
      state_->compact();
    }
  }

private:
  struct Slot {
    uint64_t id;
    std::shared_ptr<Fn> fn;
    bool live;
  };

  struct State final : detail::SignalStateBase {
    litestl::util::Vector<Slot> slots;
    uint64_t nextId = 1;
    int emitDepth = 0;
    bool needsCompact = false;

    void disconnect(uint64_t id) override
    {
      for (Slot &slot : slots) {
        if (slot.id == id && slot.live) {
          slot.live = false;
          needsCompact = true;
        }
      }
      if (emitDepth == 0 && needsCompact) {
        compact();
      }
    }
    bool connected(uint64_t id) const override
    {
      for (const Slot &slot : slots) {
        if (slot.id == id) {
          return slot.live;
        }
      }
      return false;
    }
    void compact()
    {
      litestl::util::Vector<Slot> kept;
      for (Slot &slot : slots) {
        if (slot.live) {
          kept.append(std::move(slot));
        }
      }
      slots = std::move(kept);
      needsCompact = false;
    }
  };

  std::shared_ptr<State> state_;
};

} // namespace coredcc

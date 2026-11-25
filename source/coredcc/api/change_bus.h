#pragma once

/** \file
 * ChangeBus: change notifications for datapath watchers (architecture §5, "ChangeBus").
 */

#include "coredcc/api/context.h"
#include "coredcc/base/signal.h"
#include "coredcc/parse/path.h"
#include "coredcc/props/value.h"

#include "litestl/util/function.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace coredcc {

/** Which aspects of an owner changed, as bits. Bits 0 to 15 are coredcc's: geometry's are
 * InvalidateKinds. The others are the client's, whose owners it is. */
using ChangeAspects = uint32_t;
inline constexpr ChangeAspects kAllAspects = ~ChangeAspects(0);

/** What a change listener is told. */
struct BusChange {
  /** Null after notifyAll: anything may have changed. */
  const OwnerKey *owner = nullptr;
  /** Empty for anything in it. */
  std::string_view member;
  ChangeAspects aspects = kAllAspects;
};

/**
 * Collects changes and tells watchers about them at `flush()`.
 *
 * - A **change** is `(owner, member)`; an empty member means "anything in owner".
 *   `setValue` posts its own; C++ that mutates data directly calls `notify`.
 * - The **structural epoch** is bumped by anything that can re-route a path without
 *   writing a member on it: a collection's active item, insert, remove or reorder.
 * - A **watcher** holds a Path and its Context. At flush, a watcher is resolved again
 *   when its last resolution touched a changed (owner, member), when the epoch moved,
 *   or when its Context's Library changed (a block created, deleted, restored or
 *   detached, or a reference written). Its callback runs only if the value differs
 *   (`PropValue ==`, so NaN doesn't refire). That re-resolution is what makes aliases
 *   work:
 *   `objects["A"].x` and `objects.active.x` both see a change to A's x.
 * - What a dynamic hook looks at isn't known, so a watcher of a member that has one is read at
 *   every flush that has changes.
 * - A path that fails to resolve reads as None with the error; it fires once on the
 *   transition, and again when it resolves.
 *
 * - A **listener** is called at once, from inside `notify` and `notifyAll`. It is for
 *   dependency tracking (graph evaluation); UIs use watchers.
 *
 * Callbacks may watch, unwatch and notify. Changes posted during a flush are delivered
 * by the next one.
 */
class ChangeBus {
 public:
  static constexpr const char *kExtName = "coredcc.ChangeBus";
  using WatchFn = litestl::util::function<void(const PropValue &value, const Status &status)>;
  /** For what `getMeta` gives for a path, or the struct it is in: it may differ now. */
  using MetaFn = litestl::util::function<void()>;
  /** `owner` is what changed; null after notifyAll (anything may have). */
  using ListenFn = litestl::util::function<void(const OwnerKey *owner)>;
  using ChangeFn = litestl::util::function<void(const BusChange &change)>;

  explicit ChangeBus(Runtime *rt);
  ~ChangeBus();
  ChangeBus(const ChangeBus &) = delete;
  ChangeBus &operator=(const ChangeBus &) = delete;

  /** A change to `owner`'s member, or with no member to anything in it. Calls the
   * listeners now. A null owner is ignored. `aspects` are for change listeners: watchers
   * don't go by them. */
  void notify(const OwnerKey &owner,
              std::string_view member = {},
              ChangeAspects aspects = kAllAspects);
  /** Anything may have changed (an opaque snapshot was restored): every watcher is read
   * again at the next flush, and the listeners are called now, with null. */
  void notifyAll();
  /** Re-reads every watcher at the next flush. It says nothing about stored data, so
   * listeners aren't called. */
  void bumpEpoch();
  uint64_t epoch() const;
  /** Changes waiting for the next flush. */
  int pendingCount() const;

  /** Watches `path` from `ctx`, which must outlive the watcher, with the prefix that `ctx` has now.
   * The current value is
   * read now and does not fire. `meta` runs, before `fn`, at a flush that finds another def:
   * the path leads to another member, or stopped or started resolving, or a dynamic hook gives
   * another range, say. */
  Result<ScopedConnection> watch(Context &ctx, const Path &path, WatchFn fn, MetaFn meta = {});
  Result<ScopedConnection> watch(Context &ctx,
                                 std::string_view path,
                                 WatchFn fn,
                                 MetaFn meta = {});
  int watcherCount() const;

  /**
   * Calls `fn` on every notify and notifyAll, until the connection goes. The change is
   * already in the data. A listener may connect, disconnect and notify (which calls the
   * listeners again, itself included). It must not write the document, or destroy the
   * Runtime.
   */
  ScopedConnection addListener(ListenFn fn);
  /** The same, with all that is known about the change. */
  ScopedConnection addChangeListener(ChangeFn fn);

  /** Delivers pending changes; returns how many watchers fired. */
  int flush();

  Status validate() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace coredcc

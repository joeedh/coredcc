#pragma once

/** \file
 * The Library: the block records of one document, their states, references and
 * garbage collection (architecture §8a).
 */

#include "coredcc/base/handle_pool.h"
#include "coredcc/base/runtime.h"
#include "coredcc/lib/block.h"
#include "coredcc/lib/registry.h"

#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/**
 * The block records of one document. The host creates Libraries (a Runtime may have
 * several) and destroys them before their Runtime. Not thread-safe.
 *
 * **Busy rules.** While a sweep runs (collect(), destroyFromHistory(), the destructor),
 * providers' `destroy` may call back in: reads work, void mutations are deferred
 * until the sweep ends, and calls that return a Status fail with Busy.
 */
class Library {
 public:
  explicit Library(Runtime *rt);
  ~Library();
  Library(const Library &) = delete;
  Library &operator=(const Library &) = delete;

  Runtime &runtime() const
  {
    return rt_;
  }

  /* ---- creating ---- */

  /** A new block of `type` with a fresh LocalId from the Runtime's GUID source, under
   * the type's client. */
  Result<BlockHandle> create(TypeId type, std::string_view name, void *clientData);
  /** A new block with an id the client built (unique within the client: salt it per
   * document). AlreadyExists if a record has the id. */
  Result<BlockHandle> createWithId(TypeId type,
                                   BlockId id,
                                   std::string_view name,
                                   void *clientData);
  /**
   * Recreates a block under an id it had before (redo of a create). An unknown id
   * creates a record. A Live id is AlreadyExists; an InHistory, Detached or Placeholder
   * record is FailedPrecondition: restore those instead.
   */
  Result<BlockHandle> reviveAs(TypeId type, BlockId id, std::string_view name, void *clientData);
  /** A record for a block whose client or type is missing; it keeps the type name and
   * payload so the file round-trips. */
  Result<BlockHandle> addPlaceholder(BlockId id,
                                     std::string_view typeName,
                                     std::string_view name,
                                     litestl::util::Vector<uint8_t> payload);

  /* ---- finding ---- */

  /** The record for `id` (its client normalized through aliases), or a null handle. */
  BlockHandle find(BlockId id) const;
  const Datablock *get(BlockHandle h) const;
  int size() const
  {
    return pool_.size();
  }
  /** Calls fn(BlockHandle, const Datablock&) for every record. */
  template <class Fn> void forEach(Fn &&fn) const
  {
    const_cast<HandlePool<Datablock> &>(pool_).forEach(
        [&fn](BlockHandle h, Datablock &b) { fn(h, static_cast<const Datablock &>(b)); });
  }

  /* ---- states ---- */

  /** Live -> InHistory: deleted, restorable by undo. */
  Status markDeleted(BlockHandle h);
  /** InHistory -> Live. */
  Status restoreFromHistory(BlockHandle h);
  /** Destroys an InHistory block now, when the history drops the step that holds it. */
  Status destroyFromHistory(BlockHandle h);
  /** The host destroyed the data: Live -> Detached. */
  Status onHostDestroyed(BlockId id);
  /** The host brought it back: Detached -> Live. */
  Status onHostRestored(BlockId id);

  /* ---- flags and pins (void: deferred during a sweep) ---- */

  void setFlag(BlockHandle h, BlockFlags flag, bool on);
  /** Host pins are GC roots: graph payloads and evaluated state pin blocks whose data
   * they alias. */
  void pin(BlockHandle h);
  void unpin(BlockHandle h);

  /* ---- data and references ---- */

  /** The block's data through its provider, or null unless the block is Live. */
  void *data(BlockHandle h) const;
  /**
   * The Live block `cell` points to, or a null handle. Uses the cached handle when it
   * still names a record with the same BlockId (handles alone can alias across
   * Libraries), and refreshes it otherwise.
   */
  BlockHandle resolveRef(const RefCell &cell) const;
  void *resolveData(const RefCell &cell) const
  {
    return data(resolveRef(cell));
  }
  /** Sets `cell`'s target and bumps the epoch. A Strong cell can't be set to null. */
  Status writeRef(RefCell &cell, BlockId target);
  /** Bumps the epoch after references changed without writeRef (a journal undo, a
   * snapshot restore, a host transaction). */
  void noteRefsChanged();
  uint64_t epoch() const
  {
    return epoch_;
  }

  /* ---- ref slots: element-level references held by the record ---- */

  /** Sets slot (ns, key) on `owner`. `ns` is the owning provider's or attachment's type,
   * so two clients' slots can't collide. */
  Status setSlot(BlockHandle owner, TypeId ns, int64_t key, BlockId target, RefKind kind);
  /** The slot, or null. Valid until the next setSlot or forceUnlink on `owner`. */
  const RefCell *slot(BlockHandle owner, TypeId ns, int64_t key) const;
  /** Removes a slot. Also how a placeholder's refs are cut (forceUnlink). */
  Status forceUnlink(BlockHandle owner, TypeId ns, int64_t key);

  /** Every reference out of `h`: the provider's, then the slots. */
  Status foreachRef(BlockHandle h, litestl::util::function<void(RefCell &)> fn) const;
  /** Blocks with a reference (of any kind) to `target`. Cached by epoch. */
  litestl::util::Vector<BlockHandle> usersOf(BlockHandle target) const;

  /* ---- interfaces ---- */

  /**
   * Interface I of block `h`, or a null interface. Checks the block's override table
   * (if it has one), then the implementation registered for the block type with the
   * block's data as `self`, then the provider's queryInterface hook.
   */
  template <class I> I query(BlockHandle h) const
  {
    I out;
    static_cast<IfaceRef &>(out) = queryRaw(h, InterfaceId::of(I::ifaceName));
    return out;
  }
  IfaceRef queryRaw(BlockHandle h, InterfaceId iface) const;
  /** A per-block implementation of `iface`, ahead of the type's. `ref` must stay valid
   * while set. */
  Status setOverride(BlockHandle h, InterfaceId iface, IfaceRef ref);
  void clearOverride(BlockHandle h, InterfaceId iface);

  /* ---- attachments ---- */

  /** Client data attached to a block under `type`; `destroy` frees it with the block.
   * The undoable attach and detach commands are in command/. */
  Status attach(BlockHandle h, TypeId type, void *data, void (*destroy)(void *data));
  /** Removes an attachment without destroying it, returning its data. */
  Result<void *> detach(BlockHandle h, TypeId type);
  void *attachment(BlockHandle h, TypeId type) const;

  /* ---- garbage collection ---- */

  /**
   * Mark and sweep (architecture §8a). Roots: Root and FakeUser blocks, pinned blocks,
   * and every block that isn't Live (the history, the host or a file still holds it).
   * Marks along strong references, from providers and slots. Returns how many blocks
   * were destroyed.
   *
   * Busy while a no-collect scope is open, or during a sweep. A sticky Runtime error
   * after a provider call (a foreign foreachRef that threw) aborts before anything is
   * destroyed, and is returned. With the relations audit on, a stale relations cache (a
   * reference written without the library seeing it) is an Internal error.
   */
  Result<int> collect();
  /** Interactive commands and evaluation open these; collect() is Busy inside them. */
  void beginNoCollect()
  {
    noCollect_++;
  }
  void endNoCollect();
  /** Check the relations cache against a fresh enumeration in collect(). On by default
   * in debug builds. */
  void setAuditRelations(bool on)
  {
    audit_ = on;
  }
  bool sweeping() const
  {
    return sweeping_;
  }

  /** Never reuse a freed record's slot, so a stale handle always resolves to null. For
   * tests. */
  void setQuarantine(bool on)
  {
    pool_.setQuarantine(on);
  }

  /** At most one record per BlockId, and the index matches the records. */
  Status validate() const;

 private:
  Datablock *rec(BlockHandle h)
  {
    return pool_.get(h);
  }
  const Datablock *rec(BlockHandle h) const
  {
    return pool_.get(h);
  }
  Result<BlockHandle> addRecord(TypeId type, BlockId id, std::string_view name, void *clientData);
  Status busyIfSweeping(const char *what) const;
  /** Runs `fn` now, or after the sweep if one is running. */
  void runOrDefer(litestl::util::function<void()> fn);
  void buildRelations(litestl::util::Map<BlockId, litestl::util::Vector<BlockHandle>> &out,
                      Status &status) const;
  /** Two-phase destruction of `garbage` (see collect). */
  void destroyBlocks(const litestl::util::Vector<BlockHandle> &garbage);

  Runtime &rt_;
  BlockTypeRegistry &types_;
  ClientRegistry &clients_;
  HandlePool<Datablock> pool_;
  litestl::util::Map<BlockId, BlockHandle> byId_;
  uint64_t epoch_ = 1;
  int noCollect_ = 0;
  bool sweeping_ = false;
  /** Set for the whole of collect(), mark phase included. */
  bool collecting_ = false;
  /** The ClientRegistry generation the id index was built against. */
  mutable uint64_t clientGeneration_ = 0;
  void renormalizeIds() const;
  bool audit_;
  litestl::util::Vector<litestl::util::function<void()>> deferred_;

  mutable uint64_t relationsEpoch_ = 0;
  mutable litestl::util::Map<BlockId, litestl::util::Vector<BlockHandle>> relations_;
};

template <class T, RefKind K> T *TypedRef<T, K>::get(const Library &lib) const
{
  return static_cast<T *>(lib.resolveData(cell_));
}

} // namespace coredcc

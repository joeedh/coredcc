#pragma once

/** \file
 * The Library: the block records of one document, their states, references and
 * garbage collection (architecture §8a).
 */

#include "coredcc/base/handle_pool.h"
#include "coredcc/base/name_table.h"
#include "coredcc/base/runtime.h"
#include "coredcc/lib/block.h"
#include "coredcc/lib/id_key.h"
#include "coredcc/lib/registry.h"

#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>
#include <string_view>

namespace coredcc {

class AttachmentTypeRegistry;
class LibraryRef;
namespace detail {
struct LibraryToken;
}

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
  /** A weak reference, for code that may outlive this Library (coredcc/lib/pin.h). */
  LibraryRef ref() const;
  /** Names that files brought in and the Runtime doesn't know (a missing add-on's structs
   * and runtime owners), so a save can write them again. */
  NameTable &fileNames()
  {
    return fileNames_;
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
   * creates a record, and a MissingBlock placeholder is filled (createWithId). A Live id is
   * AlreadyExists; any other record is FailedPrecondition: restore it instead.
   */
  Result<BlockHandle> reviveAs(TypeId type, BlockId id, std::string_view name, void *clientData);
  /** A record for a block whose type or data is missing (see PlaceholderReason). */
  Result<BlockHandle> addPlaceholder(BlockId id, PlaceholderInfo info);
  /** A MissingType placeholder without references. */
  Result<BlockHandle> addPlaceholder(BlockId id,
                                     std::string_view typeName,
                                     std::string_view name,
                                     litestl::util::Vector<uint8_t> payload);

  /* ---- finding ---- */

  /** The record for `id` (its client normalized through aliases), or a null handle. */
  BlockHandle find(BlockId id) const;
  /** The record, Doomed ones included. */
  const Datablock *get(BlockHandle h) const;
  /** Records with an id: Doomed ones aren't counted. */
  int size() const
  {
    return pool_.size() - doomedCount_;
  }
  /** Calls fn(BlockHandle, const Datablock&) for every record that isn't Doomed. */
  template <class Fn> void forEach(Fn &&fn) const
  {
    const_cast<HandlePool<Datablock> &>(pool_).forEach([&fn](BlockHandle h, Datablock &b) {
      if (b.state_ != BlockState::Doomed) {
        fn(h, static_cast<const Datablock &>(b));
      }
    });
  }

  /* ---- states ---- */

  /** Live, Placeholder or Detached -> InHistory: deleted, restorable by undo. */
  Status markDeleted(BlockHandle h);
  /** InHistory -> the state it was deleted in. */
  Status restoreFromHistory(BlockHandle h);
  /** Changes the name. Names aren't unique. */
  Status rename(BlockHandle h, std::string_view name);
  /**
   * Destroys an InHistory block, when the history drops the step that holds it. Refused
   * (FailedPrecondition) while a Live, Detached or Placeholder block holds a Strong ref
   * to it.
   *
   * With Host pins it returns ok and the block becomes Doomed instead: its id is free at
   * once, and it is destroyed when the last Host pin goes (or when the sweep or
   * no-collect scope that happens in ends). If such a block holds a Strong ref to
   * its id by then, and no other record has the id, it goes back to InHistory.
   * History pins don't matter here: the step holding one can't undo past this one.
   */
  Status destroyFromHistory(BlockHandle h);
  /** The host destroyed the data: Live -> Detached. On an InHistory block it changes the
   * state restoring goes back to. */
  Status onHostDestroyed(BlockId id);
  /** The host brought it back: Detached -> Live, also of an InHistory block. */
  Status onHostRestored(BlockId id);

  /* ---- flags and pins (void: deferred during a sweep) ---- */

  void setFlag(BlockHandle h, BlockFlags flag, bool on);
  /** Pins are GC roots (see PinKind). LibraryPin (coredcc/lib/pin.h) is the RAII form.
   * A handle whose record is gone is ignored; unpinning a block that has no pin of that
   * kind aborts. Main thread only. */
  void pin(BlockHandle h, PinKind kind = PinKind::Host);
  void unpin(BlockHandle h, PinKind kind = PinKind::Host);

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
  /** Removes a slot. */
  Status forceUnlink(BlockHandle owner, TypeId ns, int64_t key);
  /** Calls fn(TypeId ns, int64_t key, const RefCell &) for each slot. `fn` must not change
   * them. */
  template <class Fn> void forEachSlot(BlockHandle owner, Fn &&fn) const
  {
    if (const Datablock *b = rec(owner)) {
      for (const Datablock::Slot &s : b->slots_) {
        fn(s.ns, s.key, s.cell);
      }
    }
  }

  /* ---- a placeholder's references ---- */

  /** Nulls row `index` of a placeholder's ref table (its indices stay valid), so a block it
   * references can be deleted. */
  Status forceUnlinkRef(BlockHandle placeholder, int index)
  {
    return relinkRef(placeholder, index, BlockId());
  }
  /** Sets a row again: the undo of forceUnlinkRef. */
  Status relinkRef(BlockHandle placeholder, int index, BlockId target);

  /** Every reference out of `h`: the provider's, the slots', then the attachments'. */
  Status foreachRef(BlockHandle h, litestl::util::function<void(RefCell &)> fn) const;
  /** The references in the block's data alone, as its provider reports them. */
  Status foreachDataRef(BlockHandle h, litestl::util::function<void(RefCell &)> fn) const;
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
  /** Calls fn(InterfaceId, const IfaceRef &) for each override. `fn` must not change them. */
  template <class Fn> void forEachOverride(BlockHandle h, Fn &&fn) const
  {
    if (const Datablock *b = rec(h)) {
      for (const Datablock::Override &o : b->overrides_) {
        fn(o.iface, o.ref);
      }
    }
  }

  /* ---- attachments ---- */

  /** Attaches client data to a block under `type`, which must be in the
   * AttachmentTypeRegistry; its provider destroys the data with the block. The undoable
   * attach and detach commands are in command/. */
  Status attach(BlockHandle h, TypeId type, void *data);
  /** Removes an attachment without destroying it, returning its data. */
  Result<void *> detach(BlockHandle h, TypeId type);
  void *attachment(BlockHandle h, TypeId type) const;
  /** Calls fn(TypeId, void *data) for each attachment. `fn` must not change them. */
  template <class Fn> void forEachAttachment(BlockHandle h, Fn &&fn) const
  {
    if (const Datablock *b = rec(h)) {
      for (const Datablock::Attachment &a : b->attachments_) {
        fn(a.type, a.data);
      }
    }
  }

  /* ---- garbage collection ---- */

  /**
   * Mark and sweep (architecture §8a). Roots: Root and FakeUser blocks, pinned blocks,
   * and every block that isn't Live (the history, the host or a file still holds it),
   * except a MissingBlock that was never a record.
   * Marks along strong references, from providers and slots. Returns how many blocks
   * were destroyed.
   *
   * Busy while a no-collect scope is open, or during a sweep. A sticky Runtime error
   * after a provider call (a foreign foreachRef that threw) aborts before anything is
   * destroyed, and is returned. With the relations audit on, a stale relations cache (a
   * reference written without the library seeing it) is an Internal error.
   *
   * Errors from the deferred destruction of Doomed blocks have no caller to go to. They
   * don't become the Runtime's sticky error: the next collect() returns them, after its
   * own work.
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
  bool auditsRelations() const
  {
    return audit_;
  }
  bool sweeping() const
  {
    return sweeping_;
  }
  /** Sweeping or collecting: calls that return a Status fail with Busy. */
  bool busy() const
  {
    return sweeping_ || collecting_;
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
  friend Status readPayload(Library &lib, BlockHandle h, const BlockPayload &payload);

  Datablock *rec(BlockHandle h)
  {
    return pool_.get(h);
  }
  const Datablock *rec(BlockHandle h) const
  {
    return pool_.get(h);
  }
  /** Fills a MissingBlock placeholder in place. */
  Result<BlockHandle> adopt(BlockHandle h,
                            const BlockTypeRegistry::Entry &type,
                            std::string_view name,
                            void *clientData);
  /** A key for maps over block ids; only for this Library's maps. */
  BlockIdKey keyOf(const BlockId &id) const
  {
    return hasher_.key(id);
  }
  Result<BlockHandle> addRecord(TypeId type, BlockId id, std::string_view name, void *clientData);
  Status busyIfSweeping(const char *what) const;
  /** Runs `fn` now, or after the sweep if one is running. */
  void runOrDefer(litestl::util::function<void()> fn);
  using Relations = litestl::util::Map<BlockIdKey, litestl::util::Vector<BlockHandle>>;
  static bool sameRelations(const Relations &a, const Relations &b);
  void buildRelations(Relations &out,
                      Status &status) const;
  /** Two-phase destruction of `garbage` (see collect). */
  void destroyBlocks(const litestl::util::Vector<BlockHandle> &garbage);
  /** Tells weak users outside `lost` that its blocks go. Doomed ones were told before. */
  void noticeWeakLost(const litestl::util::Vector<BlockHandle> &lost);
  void setState(Datablock &b, BlockState state)
  {
    b.state_ = state;
    b.stateChanges_++;
    epoch_++;
  }
  /** A Live, Detached or Placeholder block with a Strong ref to `id`, or null. */
  const Datablock *strongUserOf(BlockId id) const;
  Status doom(BlockHandle h);
  /** onHostDestroyed and onHostRestored: the data's state goes from `from` to `to`. */
  Status hostChanged(Datablock &b, const char *op, BlockState from, BlockState to);
  /** Destroys the Doomed blocks that lost their last Host pin, unless the library is busy
   * or in a no-collect scope. */
  void releaseDoomed();
  void releaseOneDoomed(BlockHandle h);
  Status takeDeferredErrors();
  /** Runs what was deferred during a sweep. */
  void runDeferred();
  /** readPayload's end: makes `data` the record's, or has the provider destroy it. */
  void adoptData(BlockHandle h, void *data, bool keep);
  /** Calls `visit` with a visitor over `fn`, and turns a new sticky error into a Status. */
  Status visitRefs(const Datablock &b,
                   litestl::util::function<void(RefCell &)> &fn,
                   litestl::util::function<void(RefVisitor &)> visit) const;

  Runtime &rt_;
  BlockTypeRegistry &types_;
  ClientRegistry &clients_;
  AttachmentTypeRegistry &attachmentTypes_;
  NameTable fileNames_;
  HandlePool<Datablock> pool_;
  BlockIdHasher hasher_;
  litestl::util::Map<BlockIdKey, BlockHandle> byId_;
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
  std::shared_ptr<detail::LibraryToken> token_;

  int doomedCount_ = 0;
  bool releasing_ = false;
  /** Doomed blocks without Host pins, waiting for releaseDoomed. */
  litestl::util::Vector<BlockHandle> released_;
  /** Errors of deferred destructions: the first few, and how many there were. */
  litestl::util::Vector<Status> deferredErrors_;
  int deferredErrorCount_ = 0;

  mutable uint64_t relationsEpoch_ = 0;
  mutable Relations relations_;
};

template <class T, RefKind K> T *TypedRef<T, K>::get(const Library &lib) const
{
  return static_cast<T *>(lib.resolveData(cell_));
}

} // namespace coredcc

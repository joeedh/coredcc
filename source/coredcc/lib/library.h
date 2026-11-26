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
#include <span>
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
 * until the sweep ends, and calls that return a Status fail with Busy. The same holds in
 * providers' slots while a load, a save or a duplication runs.
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

  const NameTable &fileNames() const
  {
    return fileNames_;
  }

  /* ---- creating ---- */

  /** A new block of `type` with a fresh LocalId from the Runtime's GUID source, under
   * the type's client. */
  Result<BlockHandle> create(TypeId type, std::string_view name, void *clientData);
  /** A new block with an id the client built (unique within the client: salt it per
   * document). AlreadyExists if a record has the id, except a MissingBlock of no type or
   * this one, which is filled in place: it keeps its slots, attachments and flags. */
  Result<BlockHandle>
  createWithId(TypeId type, BlockId id, std::string_view name, void *clientData);
  /**
   * Recreates a block under an id it had before (redo of a create). An unknown id
   * creates a record, and a MissingBlock placeholder is filled (createWithId). A Live id
   * is AlreadyExists; any other record is FailedPrecondition: restore it instead.
   */
  Result<BlockHandle>
  reviveAs(TypeId type, BlockId id, std::string_view name, void *clientData);
  /** Whether `h` was a MissingBlock that createWithId or an append filled. */
  bool adopted(BlockHandle h) const;
  /**
   * The undo of filling a MissingBlock: `h` is one again, as it was, and what came since
   * (the data, slots, attachments, flags) moves to a Doomed record, which is the result.
   * Pin that (PinKind::Host, LibraryPin): its data is destroyed when the last pin goes,
   * or when those that were on `h` go. FailedPrecondition when `h` is deleted or wasn't
   * adopted.
   */
  Result<BlockHandle> unadopt(BlockHandle h);
  /** The redo: moves it back into `h`. FailedPrecondition when `h` isn't missing any
   * more, NotFound when the data is gone. */
  Status readopt(BlockHandle h, BlockHandle kept);
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
    const_cast<HandlePool<Datablock> &>(pool_).forEach(
        [&fn](BlockHandle h, Datablock &b) {
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
  /** The same for many, in one pass over the library. Those that can't be destroyed stay;
   * the result is the first error. */
  Status destroyFromHistory(std::span<const BlockHandle> blocks);
  /** The host destroyed the data: Live -> Detached. On an InHistory block it changes the
   * state restoring goes back to. */
  Status onHostDestroyed(BlockId id);
  /** The host brought it back: Detached -> Live, also of an InHistory block. */
  Status onHostRestored(BlockId id);
  /** `fn` is called with each block that onHostRestored or restoreFromHistory made Live.
   * Listeners go with the Library. */
  void addReviveListener(litestl::util::function<void(Library &, BlockHandle)> fn)
  {
    reviveListeners_.append(std::move(fn));
  }

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
  /** The slot, or null. Valid until the next setSlot or removeSlot on `owner`. */
  const RefCell *slot(BlockHandle owner, TypeId ns, int64_t key) const;
  /** Removes a slot. */
  Status removeSlot(BlockHandle owner, TypeId ns, int64_t key);
  /** Calls fn(TypeId ns, int64_t key, const RefCell &) for each slot. `fn` must not
   * change them. */
  template <class Fn> void forEachSlot(BlockHandle owner, Fn &&fn) const
  {
    if (const Datablock *b = rec(owner)) {
      for (const Datablock::Slot &s : b->slots_) {
        fn(s.ns, s.key, s.cell);
      }
    }
  }

  /* ---- the ref tables of data this build can't read ---- */

  /** The ref table of a placeholder, or of `h`'s opaque attachment of the given type;
   * null when there is none. Valid until the block changes. */
  const litestl::util::Vector<RefCell> *refTable(BlockHandle h,
                                                 TypeId attachment = {}) const;
  /** Nulls row `index` of a table (its indices stay valid), so a block it references can
   * be deleted. */
  Status unlinkRef(BlockHandle h, int index, TypeId attachment = {})
  {
    return relinkRef(h, index, BlockId(), attachment);
  }
  /** Sets a row again: the undo of unlinkRef. */
  Status relinkRef(BlockHandle h, int index, BlockId target, TypeId attachment = {});

  /** Every reference out of `h`: the provider's, the slots', then the attachments'
   * (opaque ones included). */
  Status forEachRef(BlockHandle h, litestl::util::function<void(RefCell &)> fn) const;
  /** The references in the block's data alone, as its provider reports them. */
  Status forEachDataRef(BlockHandle h, litestl::util::function<void(RefCell &)> fn) const;
  /** Blocks with a reference (of any kind) to `target`. Cached by epoch. */
  litestl::util::Vector<BlockHandle> usersOf(BlockHandle target) const;

  /* ---- interfaces ---- */

  /**
   * Interface I of block `h`, or a null interface. Checks the block's override table
   * (if it has one), then the implementation registered for the block type with the
   * block's data as `self`, then the provider's queryInterface hook.
   *
   * What a block answers may change only with a ChangeBus notification of the block or a
   * move of the Library's epoch (setOverride moves it). Evaluated graphs look at the
   * answer again only then, and would keep a stale one otherwise.
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
  /** Calls fn(InterfaceId, const IfaceRef &) for each override. `fn` must not change
   * them. */
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
   * AttachmentTypeRegistry; its provider destroys the data with the block. AlreadyExists
   * if the block has one, read or opaque. `data` isn't null, also for a tag. On an error
   * the data stays the caller's. The undoable attach and detach commands are in command/.
   */
  Status attach(BlockHandle h, TypeId type, void *data);
  /** Removes an attachment without destroying it, returning its data. */
  Result<void *> detach(BlockHandle h, TypeId type);
  void *attachment(BlockHandle h, TypeId type) const;
  /** Whether the block has an attachment of `type`, read or opaque. */
  bool hasAttachment(BlockHandle h, TypeId type) const;
  /** Removes an attachment that this build couldn't read. */
  Result<OpaqueAttachment> detachOpaque(BlockHandle h, TypeId type);
  /** Puts one back. AlreadyExists if the block has one of its type. */
  Status attachOpaque(BlockHandle h, OpaqueAttachment attachment);
  /** Calls fn(TypeId, void *data) for each attachment. `fn` must not change them. */
  template <class Fn> void forEachAttachment(BlockHandle h, Fn &&fn) const
  {
    if (const Datablock *b = rec(h)) {
      for (const Datablock::Attachment &a : b->attachments_) {
        fn(a.type, a.data);
      }
    }
  }

  /** Calls fn(const OpaqueAttachment &) for each attachment that this build couldn't
   * read. */
  template <class Fn> void forEachOpaqueAttachment(BlockHandle h, Fn &&fn) const
  {
    const Datablock *b = rec(h);
    if (b && b->opaque_) {
      for (const OpaqueAttachment &o : *b->opaque_) {
        fn(o);
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
   * after a provider call (a foreign forEachRef that threw) aborts before anything is
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
  /** How many records have gone, by sweeps and destroyFromHistory. While it is the same,
   * a reference to a block that had a record still has one. */
  uint64_t removedCount() const
  {
    return removed_;
  }
  /** Sweeping, collecting, loading, saving or duplicating: calls that return a Status
   * fail with Busy. */
  bool busy() const
  {
    return sweeping_ || collecting_ || loading_;
  }

  /** Never reuse a freed record's slot, so a stale handle always resolves to null. For
   * tests. */
  void setQuarantine(bool on)
  {
    pool_.setQuarantine(on);
  }

  /** What `h` takes: its record, and its data if the provider says
   * (BlockProvider::byteSize). */
  uint64_t byteSize(BlockHandle h) const;

  /** At most one record per BlockId, and the index matches the records. */
  Status validate() const;

private:
  friend class detail::LibraryFiller;

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
  /** Notes what `b`, a MissingBlock, is, before it is filled. */
  void keepMissing(Datablock &b);
  void splitAdopted(Datablock &from, Datablock &to);
  /** The last Host pin on `b` is gone. */
  void releaseLent(Datablock &b);
  /** A key for maps over block ids; only for this Library's maps. */
  BlockIdKey keyOf(const BlockId &id) const
  {
    return hasher_.key(id);
  }
  Result<BlockHandle>
  addRecord(TypeId type, BlockId id, std::string_view name, void *clientData);
  Status busyIfSweeping(const char *what) const;
  /** Runs `fn` now, or after the sweep if one is running. */
  void runOrDefer(litestl::util::function<void()> fn);
  using Relations = litestl::util::Map<BlockIdKey, litestl::util::Vector<BlockHandle>>;
  static bool sameRelations(const Relations &a, const Relations &b);
  void buildRelations(Relations &out, Status &status) const;
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
  /** For each block with a Strong user outside the history, that user. */
  litestl::util::Map<BlockIdKey, const Datablock *>
  strongUsersOf(std::span<const BlockHandle> blocks) const;
  void doom(const litestl::util::Vector<BlockHandle> &blocks);
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
  /** Calls `visit` with a visitor over `fn`, and turns a new sticky error into a Status.
   */
  Status forEachRefWith(const Datablock &b,
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
  litestl::util::Vector<litestl::util::function<void(Library &, BlockHandle)>>
      reviveListeners_;
  void revived(BlockHandle h);
  std::shared_ptr<detail::LibraryToken> token_;

  int doomedCount_ = 0;
  uint64_t removed_ = 0;
  bool releasing_ = false;
  /** A load, a save or a duplication is calling providers. */
  bool loading_ = false;
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

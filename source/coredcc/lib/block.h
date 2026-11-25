#pragma once

/** \file
 * The datablock record, references between blocks, and the visitor providers report
 * references through (architecture §8a).
 */

#include "coredcc/base/block_id.h"
#include "coredcc/base/flags.h"
#include "coredcc/base/handle_pool.h"
#include "coredcc/base/id.h"
#include "coredcc/base/iface/iface.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <memory>

namespace coredcc {

class Datablock;
class Library;
namespace detail {
class LibraryFiller;
}

/** A session-local, generational handle to a block record in one Library. Persistent
 * and undo data store BlockIds instead. */
using BlockHandle = Handle<Datablock>;

enum class BlockState : uint8_t {
  Live,
  /** Deleted, restorable by undo. The history owns the data. */
  InHistory,
  /** The host destroyed the data (an actor deleted in Unreal's Outliner). */
  Detached,
  /** A missing client or type: the record keeps the type name and payload bytes. */
  Placeholder,
  /**
   * Dropped by the history while Host pins still hold its data. It has no id any more
   * (another record may take it), so only its handle reaches it; it goes when its last
   * Host pin does. Listings, relations and files skip it.
   */
  Doomed,
};

const char *blockStateName(BlockState state);

/** Who keeps a block. Both kinds are GC roots; only Host pins also keep a block's data
 * past destroyFromHistory, and make it a root of a saved file. */
enum class PinKind : uint8_t {
  /** Something using the block's data now: a graph payload, evaluated state, a host. */
  Host,
  /** An undo record that can bring back a reference to the block. */
  History,
};

enum class BlockFlags : uint32_t {
  None = 0,
  /** A GC root. */
  Root = 1 << 0,
  /** Kept with no users, as Blender's fake user. A GC root. */
  FakeUser = 1 << 1,
  /** The block has per-block interface overrides; query() checks them first. */
  HasOverrides = 1 << 2,
};
CDCC_FLAG_OPERATORS(BlockFlags)

/** Strength and nullability of a reference. */
enum class RefKind : uint8_t {
  /** Strong and meant to be non-null: UIs and the delete cascade treat a user whose
   * Strong ref would break as going with its target. Not enforced at write time (a
   * loader writes refs to blocks it hasn't read yet), so it is null while unset. */
  Strong,
  /** Strong and nullable. */
  OptStrong,
  /** Not strong, nullable. Never nulled: it resolves to null while the target is
   * absent, and again to the target when it comes back. */
  Weak,
};

constexpr bool isStrong(RefKind k)
{
  return k != RefKind::Weak;
}

/**
 * One reference: the target's BlockId and a cached handle. Clients keep these inside
 * their data (foreign clients as bound RefCells in WASM memory, never JS copies), and
 * report them through BlockProvider::foreachRef. Write the target through
 * Library::writeRef so the library's relations stay current.
 */
struct RefCell {
  BlockId target;
  mutable BlockHandle cache;
  RefKind kind = RefKind::Strong;

  RefCell() = default;
  explicit RefCell(RefKind k) : kind(k) {}

  bool isNull() const
  {
    return target.isNull();
  }
  static const litestl::binding::types::Struct<RefCell> *defineBindings();
};

/** The library's form of one block's data (written and read through lib/block_io.h). */
struct BlockPayload {
  /** The block type's payloadVersion when it was written. */
  uint32_t version = 1;
  litestl::util::Vector<uint8_t> bytes;
  /** The ref table: rows in the order they were written, null ones and repeats included. */
  litestl::util::Vector<RefCell> refs;
};

/** An attachment from a file that this build can't read. It is kept, its references count,
 * and a save writes it back. */
struct OpaqueAttachment {
  litestl::util::string typeName;
  /** `version` is the attachment type's. */
  BlockPayload payload;
};

/** Why a record is a placeholder. */
enum class PlaceholderReason : uint8_t {
  /** A record from a file whose type or payload this build can't read. It keeps the payload
   * and is a GC root, so a save writes it back. */
  MissingType,
  /** A block that isn't here: the target of a strong reference that has no record, or a
   * record saved without its data (host-owned). Library::createWithId fills it in place. A
   * GC root only with a type name (it was a record). */
  MissingBlock,
};

/** What a placeholder is made from (Library::addPlaceholder). */
struct PlaceholderInfo {
  PlaceholderReason reason = PlaceholderReason::MissingType;
  /** Empty only for a MissingBlock that was never a record. */
  litestl::util::string typeName;
  litestl::util::string name;
  /** Where it came from (a file's name), for messages. */
  litestl::util::string sourceName;
  /** MissingType: the stored payload. MissingBlock: only the ref table, the references the
   * block had. */
  BlockPayload payload;
};

/** A typed reference. The library resolves it to the target's data, so `T` is the
 * target block type's data type; it isn't checked. */
template <class T, RefKind K> class TypedRef {
 public:
  TypedRef() : cell_(K) {}
  RefCell &cell()
  {
    return cell_;
  }
  const RefCell &cell() const
  {
    return cell_;
  }
  BlockId target() const
  {
    return cell_.target;
  }
  /** The target's data, or null while the target isn't Live. */
  T *get(const Library &lib) const;

 private:
  RefCell cell_;
};

template <class T> using Ref = TypedRef<T, RefKind::Strong>;
template <class T> using OptRef = TypedRef<T, RefKind::OptStrong>;
template <class T> using WeakRef = TypedRef<T, RefKind::Weak>;

/**
 * Receives a block's references from BlockProvider::foreachRef. A concrete bound
 * class, so a TS provider calls `visitor.visit(cell)` like any bound method. The cell
 * may be rewritten in place (remap, sweep).
 */
class RefVisitor {
 public:
  using Fn = void (*)(void *ctx, RefCell &cell);
  RefVisitor(Fn fn, void *ctx) : fn_(fn), ctx_(ctx) {}

  void visit(RefCell &cell)
  {
    fn_(ctx_, cell);
  }
  static const litestl::binding::types::Struct<RefVisitor> *defineBindings();

 private:
  Fn fn_;
  void *ctx_;
};

/** A provider's answer to queryInterface: the implementation type and `self` to look
 * up in the interface registry. A null implType means "not implemented". */
struct QueryAnswer {
  TypeId implType;
  void *self = nullptr;
  static const litestl::binding::types::Struct<QueryAnswer> *defineBindings();
};

/**
 * The library's record of one block. Clients never subclass it; its data lives
 * wherever the client keeps it, reached through BlockProvider::data. The Library owns
 * and changes records; everyone else reads them.
 */
class Datablock {
 public:
  const BlockId &id() const
  {
    return id_;
  }
  /** The block type (the BlockTypeRegistry key). */
  TypeId type() const
  {
    return type_;
  }
  const litestl::util::string &name() const
  {
    return name_;
  }
  BlockState state() const
  {
    return state_;
  }
  /** What became of the data: state(), or for an InHistory or Doomed block the state it was
   * deleted in (Live, Detached or Placeholder), which restoring goes back to. */
  BlockState dataState() const
  {
    return state_ == BlockState::InHistory || state_ == BlockState::Doomed ? deletedFrom_ :
                                                                            state_;
  }
  BlockFlags flags() const
  {
    return flags_;
  }
  bool hasFlag(BlockFlags f) const
  {
    return coredcc::hasFlag(flags_, f);
  }
  /** Pins of both kinds. */
  int pins() const
  {
    return pins_[0] + pins_[1];
  }
  int pins(PinKind kind) const
  {
    return pins_[int(kind)];
  }
  /** Bumped by every state transition. Two equal readings mean the block was never
   * deleted, detached or restored in between. */
  uint64_t stateChanges() const
  {
    return stateChanges_;
  }
  /** The pointer the client gave at creation, typically what BlockProvider::data
   * returns. */
  void *clientData() const
  {
    return clientData_;
  }
  /** Placeholder only: the original type name. */
  const litestl::util::string &placeholderType() const
  {
    return placeholderType_;
  }
  const litestl::util::Vector<uint8_t> &placeholderPayload() const
  {
    return placeholderData_.bytes;
  }
  /** Placeholder only: the stored version, bytes and ref table. */
  const BlockPayload &placeholderData() const
  {
    return placeholderData_;
  }
  /** Placeholder only. */
  PlaceholderReason placeholderReason() const
  {
    return placeholderReason_;
  }
  /** Where a placeholder came from, for messages. Kept when it is filled. */
  const litestl::util::string &sourceName() const
  {
    return sourceName_;
  }

  static const litestl::binding::types::Struct<Datablock> *defineBindings();

 private:
  friend class Library;
  friend class detail::LibraryFiller;

  struct Slot {
    /** The provider or attachment type that owns the slot. */
    TypeId ns;
    int64_t key = 0;
    RefCell cell;
  };
  struct Attachment {
    TypeId type;
    void *data = nullptr;
  };
  struct Override {
    InterfaceId iface;
    IfaceRef ref;
  };

  BlockId id_;
  TypeId type_;
  litestl::util::string name_;
  BlockState state_ = BlockState::Live;
  /** InHistory and Doomed: see dataState(). */
  BlockState deletedFrom_ = BlockState::Live;
  BlockFlags flags_ = BlockFlags::None;
  uint64_t stateChanges_ = 0;
  int pins_[2] = {0, 0};
  void *clientData_ = nullptr;
  litestl::util::Vector<Slot> slots_;
  litestl::util::Vector<Attachment> attachments_;
  /** Null for none. */
  std::unique_ptr<litestl::util::Vector<OpaqueAttachment, 1>> opaque_;
  litestl::util::Vector<Override> overrides_;
  litestl::util::string placeholderType_;
  litestl::util::string sourceName_;
  PlaceholderReason placeholderReason_ = PlaceholderReason::MissingType;
  BlockPayload placeholderData_;
};

} // namespace coredcc

#pragma once

/** \file
 * What a kernel read in its last run. Internal to graph/.
 */

#include "coredcc/base/iface/iface.h"
#include "coredcc/base/runtime.h"
#include "coredcc/graph/exec_context.h"
#include "coredcc/graph/system.h"
#include "coredcc/lib/pin.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <memory>
#include <span>

namespace coredcc {

struct SocketType;

namespace graph {

/** Where an input's value comes from, at the end of its links and groups. */
struct Origin {
  enum class Kind : uint8_t {
    None,
    /** An output of a kernel node: `a` its frame's serial, `b` the node, `c` the output. */
    Kernel,
    /** A literal: `a` the frame's serial, `b` the node, `c` the hash of the key. */
    Literal,
    /** A value set on the instance: `b` the interface id. */
    External,
    /** An interface input's default: `a` the def's serial, `b` the interface id. */
    InterfaceDefault,
    /** A node type's default: `a` its stamp, `c` the input. */
    NodeDefault,
    /** A socket type's default: `a` the type's id. */
    TypeDefault,
    /** A column of a foreach's table. */
    Column,
  };
  Kind kind = Kind::None;
  uint64_t a = 0;
  int64_t b = 0;
  uint64_t c = 0;
  /** Of the value: an output's changedAt, a literal's version. */
  uint64_t version = 0;
  bool operator==(const Origin &) const = default;
};

/** The socket types a value crosses, as small numbers: equal chains have equal numbers, so
 * comparing and extending one is O(1) however long it is. */
class TypeChains {
 public:
  static constexpr int kEmpty = -1;
  /** `chain` with `type` after it. */
  int extend(int chain, const SocketType *type);
  /** What they take, about. */
  size_t bytes() const
  {
    return size_t(ids_.size()) * (sizeof(Key) + sizeof(int));
  }

 private:
  struct Key {
    int chain = 0;
    const SocketType *type = nullptr;
    uint64_t computeHash() const
    {
      return uint64_t(uintptr_t(type)) * 0x9E3779B97F4A7C15ull + uint64_t(chain + 1);
    }
    bool operator==(const Key &) const = default;
  };
  litestl::util::Map<Key, int> ids_;
};

struct TraceInput {
  Origin origin;
  int chain = TypeChains::kEmpty;
  bool operator==(const TraceInput &) const = default;
};

/** A block a kernel asked for. */
struct BlockDep {
  /** As asked for. */
  BlockId id;
  ChangeAspects aspects = kAllAspects;
  BlockHandle handle;
  BlockState state = BlockState::Live;
  /** So a destroy and a restore between two looks are seen. */
  uint64_t stateChanges = 0;
  /** A kernel may use its name. */
  uint64_t renames = 0;
  uint64_t version = 0;
  /** So that the block is followed while this is there. */
  BlockVersionPtr follows;
  bool operator==(const BlockDep &) const = default;
};

/** An interface a kernel asked for. */
struct QueryDep {
  /** Null when there is no such block. */
  BlockId id;
  BlockHandle handle;
  InterfaceId iface;
  const IfaceTable *table = nullptr;
  void *self = nullptr;
  /** As in BlockDep; of all aspects, as any notification may go with another answer. */
  BlockVersionPtr follows;
  bool operator==(const QueryDep &b) const
  {
    return id == b.id && handle == b.handle && iface == b.iface && table == b.table &&
           self == b.self;
  }
};

/** A node's type isn't part of it: a node whose type changed has new state. */
struct Trace {
  uint64_t paramVersion = 0;
  /** NodeType::kernelStamp. */
  uint64_t kernel = 0;
  litestl::util::Vector<TraceInput, 2> inputs;

  /** What most kernels have none of. */
  struct Blocks {
    /** The target of each Pointer param's cell. A Library may write a cell, so paramVersion
     * doesn't tell. */
    litestl::util::Vector<BlockId, 1> params;
    litestl::util::Vector<BlockDep, 1> blocks;
    litestl::util::Vector<QueryDep, 1> queries;
  };
  std::unique_ptr<Blocks> more;

  Blocks &blocks()
  {
    if (!more) {
      more = std::make_unique<Blocks>();
    }
    return *more;
  }
  std::span<const BlockId> paramBlocks() const
  {
    return more ? std::span<const BlockId>(more->params.data(), size_t(more->params.size())) :
                  std::span<const BlockId>();
  }
};

/** Collects the blocks and queries of many traces in one, each as it was first seen: if it
 * moves before the last of them, that is out of date, as it must be. */
class DepSet {
 public:
  void clear();
  void add(const Trace &from, Trace &to);

 private:
  struct QueryKey {
    BlockHandle handle;
    uint64_t iface = 0;
    uint64_t computeHash() const
    {
      return handle.computeHash() * 0x9E3779B97F4A7C15ull ^ iface;
    }
    bool operator==(const QueryKey &) const = default;
  };
  struct BlockKey {
    BlockId id;
    ChangeAspects aspects = kAllAspects;
    uint64_t computeHash() const
    {
      return id.computeHash() * 0x9E3779B97F4A7C15ull ^ aspects;
    }
    bool operator==(const BlockKey &) const = default;
  };
  litestl::util::Map<BlockKey, bool> blocks_;
  litestl::util::Map<QueryKey, bool> queries_;
};

/** Finds blocks, as they are now. */
class BlockReader {
 public:
  BlockReader(Runtime &rt, LibraryRef lib);
  BlockDep read(BlockId id, ChangeAspects aspects) const;
  QueryDep read(BlockHandle h, InterfaceId iface) const;
  /** Whether every block and query in `t` would still read the same. */
  bool current(const Trace &t) const;
  Library *library() const
  {
    return lib_.get();
  }

 private:
  Runtime &rt_;
  GraphSystem &system_;
  LibraryRef lib_;
};

/** Gives a kernel its blocks, and records them in a trace. */
class BlockRecorder final : public ExecBlocks {
 public:
  BlockRecorder(const BlockReader &reader, Trace &trace) : reader_(reader), trace_(trace) {}
  BlockUse useBlock(BlockId id, ChangeAspects aspects) override;
  IfaceRef queryBlock(BlockHandle h, InterfaceId iface) override;

 private:
  const BlockReader &reader_;
  Trace &trace_;
};

} // namespace graph
} // namespace coredcc

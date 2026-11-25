#pragma once

/** \file
 * What a kernel read in its last run. Internal to graph/.
 */

#include "coredcc/base/iface/iface.h"
#include "coredcc/base/runtime.h"
#include "coredcc/graph/exec_context.h"
#include "coredcc/lib/library.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <cstdint>

namespace coredcc {

class GraphSystem;
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
  BlockHandle handle;
  BlockState state = BlockState::Live;
  uint64_t version = 0;
  bool operator==(const BlockDep &) const = default;
};

/** An interface a kernel asked for. */
struct QueryDep {
  BlockHandle handle;
  InterfaceId iface;
  const IfaceTable *table = nullptr;
  void *self = nullptr;
  bool operator==(const QueryDep &) const = default;
};

/** A node's type isn't part of it: a node whose type changed has new state. */
struct Trace {
  uint64_t paramVersion = 0;
  /** The target of each Pointer param's cell. A Library may write a cell, so paramVersion
   * doesn't tell. */
  litestl::util::Vector<BlockId, 1> paramBlocks;
  litestl::util::Vector<TraceInput, 2> inputs;
  litestl::util::Vector<BlockDep, 1> blocks;
  litestl::util::Vector<QueryDep, 1> queries;
};

/** Finds blocks, as they are now. */
class BlockReader {
 public:
  BlockReader(Runtime &rt, Library *lib);
  BlockDep read(BlockId id) const;
  QueryDep read(BlockHandle h, InterfaceId iface) const;
  /** Whether every block and query in `t` would still read the same. */
  bool current(const Trace &t) const;
  Library *library() const
  {
    return lib_;
  }

 private:
  Runtime &rt_;
  GraphSystem &system_;
  Library *lib_;
};

/** Gives a kernel its blocks, and records them in a trace. */
class BlockRecorder final : public ExecBlocks {
 public:
  BlockRecorder(const BlockReader &reader, Trace &trace) : reader_(reader), trace_(trace) {}
  BlockUse useBlock(BlockId id) override;
  IfaceRef queryBlock(BlockHandle h, InterfaceId iface) override;

 private:
  const BlockReader &reader_;
  Trace &trace_;
};

} // namespace graph
} // namespace coredcc

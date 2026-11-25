#pragma once

/** \file
 * GraphSystem: versions of the blocks kernels use, and a journal of edits.
 */

#include "coredcc/api/change_bus.h"
#include "coredcc/base/runtime.h"
#include "coredcc/lib/registry.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>

namespace coredcc {

/** Something evaluation may depend on changed: what Runtime::touch was called for. */
struct GraphEditNote {
  enum class Kind : uint8_t {
    /** Anything may have. */
    All,
    /** A block, by its canonical id. */
    Block,
    /** A param or literal of a node. */
    Node,
    /** Nodes or links of a def, but none that its interface depends on, and no node went. */
    Links,
    /** Anything else of a def. */
    Def,
  };
  Kind kind = Kind::All;
  BlockId block;
  ChangeAspects aspects = kAllAspects;
  /** GraphDef::serial. */
  uint64_t def = 0;
  int64_t node = 0;
};

/** The version of aspects of a block. Who holds it has the block followed. */
struct BlockVersion {
  ChangeAspects aspects = kAllAspects;
  uint64_t version = 0;
};
using BlockVersionPtr = std::shared_ptr<const BlockVersion>;

/**
 * A version per block that some kernel used and the aspects of it that it used, moved by every
 * ChangeBus notification of some of those (and of all blocks, by notifyAll). Those count as edits (Runtime::touch), so the
 * next evaluation looks at its dependencies again. A Runtime extension.
 *
 * The last edits are kept, so that an instance can look at only what they touch. An edit that
 * isn't noted here (a plain Runtime::touch) counts as Kind::All.
 */
class GraphSystem {
 public:
  static constexpr const char *kExtName = "coredcc.GraphSystem";

  explicit GraphSystem(Runtime *rt);

  /** The version of aspects of a block, which moves with their changes while it is held. Not
   * after notifyAll, or when the block's canonical id became another: it has another one then. */
  BlockVersionPtr follow(BlockId id, ChangeAspects aspects = kAllAspects);
  /** Moves when the nodes, links, groups or interface of any GraphDef do, or one is made. */
  uint64_t structureVersion() const
  {
    return structure_;
  }
  void noteStructure(uint64_t version)
  {
    structure_ = version;
  }
  /** Runtime::touch, with what it is for. */
  uint64_t touch(const GraphEditNote &edit);
  /** The number of the next edit. Edits are numbered from 0. */
  uint64_t journalEnd() const
  {
    return end_;
  }
  /** Whether the last touch is in the journal. */
  bool journalCurrent() const
  {
    return rt_.lastEdit() == noted_;
  }
  /** Edit `number`, or null if it isn't kept any more. */
  const GraphEditNote *journal(uint64_t number) const
  {
    return number < end_ && end_ - number <= uint64_t(journal_.size()) ?
               &journal_[int(number % kJournal)] :
               nullptr;
  }
  /** Blocks followed, with some that nobody follows any more. */
  int blockCount() const
  {
    return int(versions_.size());
  }

 private:
  void changed(const BusChange &change);
  void note(const GraphEditNote &edit);
  void prune();

  Runtime &rt_;
  ClientRegistry &clients_;
  /** By canonical id. An id that became an alias leaves its entry behind, never found again;
   * its block has another version from then on, so its users run. */
  using Versions = litestl::util::Vector<std::shared_ptr<BlockVersion>, 1>;
  litestl::util::Map<BlockId, Versions> versions_;
  /** How many blocks there may be before those that nobody holds go. */
  size_t pruneAt_ = kPrune;
  static constexpr size_t kPrune = 64;
  uint64_t structure_ = 0;
  static constexpr uint64_t kJournal = 4096;
  /** A ring, once it has kJournal. */
  litestl::util::Vector<GraphEditNote> journal_;
  uint64_t end_ = 0;
  /** The last touch that is in the journal. */
  uint64_t noted_ = 0;
  ScopedConnection connection_;
};

} // namespace coredcc

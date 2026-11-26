#pragma once

/** \file
 * The adapters of users' graphs (internal to drive/): which nodes read the scene and
 * which drive channels, with their paths through nested groups.
 */

#include "coredcc/drive/node_types.h"
#include "coredcc/graph/def.h"
#include "coredcc/lib/library.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <memory>

namespace coredcc::drive {

enum class AdapterKind : uint8_t { Time, State, Drive };

/** An adapter node, as seen from the def that was scanned. */
struct Adapter {
  AdapterKind kind = AdapterKind::Time;
  /** The group nodes on the way in, outermost first; then the node. */
  litestl::util::Vector<NodeId, 2> groups;
  NodeId node;
  /** State and Drive: the object; null if the param isn't set. */
  BlockId object;
  /** Drive: the channel def's name, and the adapter's kind. */
  litestl::util::string channel;
  DriveKind driveKind = DriveKind::None;
};

/** Something about a def's adapters that can't work. */
struct AdapterIssue {
  litestl::util::Vector<NodeId, 2> groups;
  NodeId node;
  litestl::util::string message;
};

/** The interior key of an adapter's socket, as the group node of the scanned def has it:
 * `@<groups>/<node>.<socket>`. */
std::string interiorKey(const Adapter &a, std::string_view socket);

/**
 * Finds the adapters of graph blocks, through nested groups. Each def is scanned again
 * only when its `serial` or `contentVersion`, or the Library's epoch, moved; a def's list
 * is its own nodes' and those of the defs that its groups use, which are looked at each
 * time through their own entries.
 */
class AdapterScan {
public:
  /** The adapters of the graph block, and what is wrong with them. Recursion that the
   * graph allows by a direct write is cut at 64 levels, as the evaluator does. */
  void scan(const Library &lib,
            BlockHandle graph,
            litestl::util::Vector<Adapter> &out,
            litestl::util::Vector<AdapterIssue> &issues);
  /** Drops entries of defs that weren't scanned since the last call of this. */
  void prune();

private:
  struct Child {
    NodeId node;
    BlockId def;
    bool foreach = false;
  };
  struct Entry {
    uint64_t serial = 0;
    uint64_t content = 0;
    uint64_t epoch = 0;
    bool used = false;
    litestl::util::Vector<Adapter> own;
    litestl::util::Vector<AdapterIssue> issues;
    litestl::util::Vector<Child> children;
  };
  const Entry &entryOf(const Library &lib, const GraphDef &def, const BlockId &id);
  void expand(const Library &lib,
              const BlockId &id,
              const litestl::util::Vector<NodeId, 2> &prefix,
              int depth,
              bool inForeach,
              litestl::util::Vector<Adapter> &out,
              litestl::util::Vector<AdapterIssue> &issues);

  litestl::util::Map<BlockId, std::unique_ptr<Entry>> entries_;
  /** The defs on the way down, to cut a graph that contains itself. */
  litestl::util::Vector<BlockId> path_;
};

} // namespace coredcc::drive

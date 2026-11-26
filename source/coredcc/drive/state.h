#pragma once

/** \file
 * What SceneDrivers keeps (internal to drive/): the description of the generated graph,
 * the graph and its instance, and the driven values.
 */

#include "coredcc/base/signal.h"
#include "coredcc/drive/adapters.h"
#include "coredcc/drive/drive.h"
#include "coredcc/lib/pin.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>
#include <string>

namespace coredcc {

namespace drive {

/** An object with a transform node: one that a graph reads, or an ancestor of one. */
struct ObjectDesc {
  BlockId id;
  /** Its parent, if that has a node too; null for none or on a cycle. */
  BlockId parent;
  /** What `read` gave, as the kernel compares it (formatBlockId). */
  std::string parentText;
  /** The transform type's name; empty for one that isn't registered. */
  std::string type;
  bool cycle = false;
  /** Driven objects only: false if it isn't there, or isn't Transformable. */
  bool transformable = true;

  bool operator==(const ObjectDesc &b) const = default;
};

/** A listed graph, and its adapters. */
struct GraphDesc {
  BlockId id;
  litestl::util::Vector<Adapter> adapters;
};

/** What the generated graph is made from. Two that are equal give the same graph. */
struct Description {
  litestl::util::Vector<GraphDesc> graphs;
  litestl::util::Vector<ObjectDesc> objects;
  /** The driven objects, with their types, so that a channel is checked when they change.
   */
  litestl::util::Vector<ObjectDesc> driven;
  /** What the scan found wrong, as text: part of what must be the same. */
  litestl::util::Vector<std::string> notes;

  bool operator==(const Description &b) const;
};

/** What makes the description stale. */
struct SyncKey {
  uint64_t epoch = 0;
  uint64_t clients = 0;
  int tables = 0;
  int transformTypes = 0;
  int nodeTypes = 0;
  uint64_t lastEdit = 0;
  int64_t size = -1;
  uint64_t removed = 0;
  bool operator==(const SyncKey &b) const = default;
};

/** What makes evaluated values stale: the graph's pass key, and the time. */
struct EvalKey {
  uint64_t lastEdit = 0;
  uint64_t epoch = 0;
  uint64_t clients = 0;
  int tables = 0;
  double time = 0.0;
  bool operator==(const EvalKey &b) const = default;
};

/** A drive adapter's output, and what it gave last. */
struct DriveEntry {
  BlockId object;
  litestl::util::string channel;
  DriveKind kind = DriveKind::None;
  SocketHandle handle;
  double values[4] = {};
  bool ok = false;
  /** Why it has no value. */
  std::string error;
};

} // namespace drive

struct SceneDrivers::State {
  Runtime *rt = nullptr;
  LibraryRef lib;
  OverrideProvider *inner = nullptr;
  ScopedConnection listener;
  bool rescan = true;

  /* The Library's Scene records, and its placeholders, which may become scenes. */
  litestl::util::Vector<BlockHandle> scenes;
  litestl::util::Vector<BlockHandle> placeholders;
  int64_t scannedSize = -1;
  uint64_t scannedRemoved = 0;

  drive::AdapterScan scan;
  drive::Description desc;
  bool built = false;
  int rebuilds = 0;
  drive::SyncKey synced;

  std::unique_ptr<GraphDef> def;
  std::unique_ptr<GraphInstance> inst;
  int64_t timeInput = 0;
  litestl::util::Map<BlockId, NodeId> nodeOf;
  litestl::util::Map<BlockId, SocketHandle> stateOf;
  litestl::util::Vector<SocketHandle> statusHandles;
  litestl::util::Vector<drive::DriveEntry> drives;
  /** By object: its entries, and why it can't be driven (empty if it can). */
  litestl::util::Map<BlockId, litestl::util::Vector<int>> drivesOf;
  litestl::util::Map<BlockId, std::string> problems;
  litestl::util::Vector<SocketHandle> handles;
  litestl::util::Vector<DriverDiagnostic> diagnostics;
  uint64_t drivenVersion = 1;

  bool evaluated = false;
  drive::EvalKey evalKey;
  /** The last evaluation as a whole: its error stands for every driven object. */
  Status callError;

  /* sync.cc */
  Status sync(Library &lib, bool force);
  void scanScenes(Library &lib);
  drive::Description describe(Library &lib);
  void rebuild(Library &lib);

  /* provider.cc */
  Status evaluate(Library &lib, double time);
  drive::EvalKey keyOf(const Library &lib, double time) const;
  Status ensure(Library &lib, double time);
};

} // namespace coredcc

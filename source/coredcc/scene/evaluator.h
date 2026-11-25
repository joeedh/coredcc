#pragma once

/* Scene evaluation. Internal: P5's graph client replaces what run() does. */

#include "coredcc/lib/provider.h"
#include "coredcc/scene/scene.h"

namespace coredcc::detail {

/** The runtime owner of results by path. */
OwnerKey evaluatedOwner(Runtime &rt);

/** How an interface of a block is found. */
enum class CapMode : uint8_t {
  None,
  /** The block type's own table, with the block's data as `self`. */
  Table,
  /** Asked each time: an override, or the provider's queryInterface. */
  Dynamic,
};

/** What is the same for all blocks of a type. Types and tables are never removed. */
struct SceneBlockType {
  TypeId type;
  BlockProvider provider;
  bool hasQuery = false;
  const IfaceTable *transformable = nullptr;
  const IfaceTable *spatial = nullptr;
};

/** What is looked up once for an object, and its place in the hierarchy. */
struct SceneMember {
  BlockHandle handle;
  /** The data block. Null unless Live. */
  BlockHandle data;
  /** As `read` gave them; every evaluation compares them. */
  BlockId parentId;
  BlockId dataId;
  /** The parent's index, or -1. */
  int parent = -1;
  /** Into types_. */
  int type = -1;
  int dataType = -1;
  bool member = false;
  /** Whether the block's data, and the data block's, was null; every evaluation compares it. */
  bool isNull = false;
  bool dataIsNull = false;
  CapMode transformable = CapMode::None;
  CapMode spatial = CapMode::None;
  CapMode dataSpatial = CapMode::None;
};

/** Where an object's memory was in the last evaluation: its record, its data, its data block's
 * record and data. Only ever prefetched: evaluating is mostly waiting for memory. */
struct PrefetchHint {
  uintptr_t at[4] = {};
};

/** What makes lookups stale. */
struct LookupKey {
  uint64_t epoch = 0;
  int tables = 0;
  uint64_t clients = 0;

  bool operator==(const LookupKey &b) const = default;
};

class SceneEvaluator {
 public:
  Scene::Counters counters;
  SceneBvh bvh;

  /** Evaluates, and makes the results the last ones if they differ, with their BVH. Opens the EvaluationScope;
   * the no-collect is the caller's. The first foreign exception goes to `foreign`. */
  Status run(const Scene &scene, const Library &lib, bool &changed, Status &foreign);

  /** The last results. */
  const EvaluatedStore &front() const
  {
    return stores_[front_];
  }

 private:
  enum class Pass { Same, Changed, Stale };
  /** Writes the back store, if anything differs from the front one. */
  Pass evaluateAll(const Scene &scene, const Library &lib, Status &foreign);
  bool stillValid(const Scene &scene, const Library &lib) const;
  void lookUp(const Scene &scene, const Library &lib);
  /** `before` is what was found for it last time, if anything. */
  SceneMember memberFor(const Library &lib,
                        BlockHandle h,
                        bool inScene,
                        const SceneMember *before);
  int typeFor(const Library &lib, TypeId type);
  void sortByParent();

  litestl::util::Vector<SceneBlockType, 4> types_;
  litestl::util::Vector<SceneMember> members_;
  litestl::util::Vector<PrefetchHint> hints_;
  /** The first ones are the scene's members. */
  int inScene_ = 0;
  /** Parents before children. */
  litestl::util::Vector<int> order_;
  /** 1 for the members of a cycle. */
  litestl::util::Vector<uint8_t> cyclic_;
  std::shared_ptr<const HandleIndex> index_;
  LookupKey key_;
  bool valid_ = false;
  ChannelBlock scratch_;
  EvaluatedStore stores_[2];
  int front_ = 0;
};

} // namespace coredcc::detail

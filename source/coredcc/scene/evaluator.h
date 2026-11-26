#pragma once

/* Scene evaluation. Internal: P5's graph client replaces what run() does. */

#include "coredcc/base/signal.h"
#include "coredcc/lib/provider.h"
#include "coredcc/scene/overrides.h"
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
  /** It implements Invalidation: bounds are kept. */
  bool invalidation = false;
  /** coredcc.Object: the data is an Object. */
  bool object = false;
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
  /** The provider drives it (OverrideProvider::Driven), and is asked about it. */
  bool driven = false;
  /** Whether the block's data, and the data block's, was null; every evaluation compares
   * it. */
  bool isNull = false;
  bool dataIsNull = false;
  CapMode transformable = CapMode::None;
  CapMode spatial = CapMode::None;
  CapMode dataSpatial = CapMode::None;
  /** Into boxes_, if the bounds of the block that has them are kept, or -1. */
  int box = -1;
  /** How many are above it; 0 on a cycle. */
  int parentDepth = 0;
  /** The transform type it had last. */
  TypeId transformType;
};

/** The bounds of a block, until it changes. */
struct KeptBox {
  Box3d box;
  bool valid = false;
};

/** An object that is evaluated together with others of its type and level. */
struct BatchItem {
  bool done = false;
  ObjectInfo info;
  TransformState state;
  EvalStatus status = EvalStatus::Ok;
};

/** Where the memory of an object was in the last evaluation: its record, its data, the
 * record and data of its data block, and its channels, which are outside of an Object
 * with more than 16. Only ever prefetched: evaluating is mostly waiting for memory. */
struct PrefetchHint {
  uintptr_t at[5] = {};
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
  Scene::Changes changes;
  SceneBvh bvh;
  const litestl::util::Vector<BlockHandle> none;

  /** Evaluates, and makes the results the last ones if they differ, with their BVH. Opens
   * the EvaluationScope; the no-collect is the caller's. The first foreign exception goes
   * to `foreign`. */
  Status run(const Scene &scene,
             const Library &lib,
             const OverrideProvider::Driven &driven,
             bool &changed,
             Status &foreign);
  /** Of the last results. */
  std::span<const BlockHandle> children(BlockHandle h) const;
  const litestl::util::Vector<BlockHandle> &recorded(const Library &lib) const;

  /** The last results. */
  const EvaluatedStore &front() const
  {
    return stores_[front_];
  }

private:
  enum class Pass { Same, Changed, Stale };
  /** Writes the back store, if anything differs from the front one. `incremental`: only
   * what is dirty, below what moved, or has bounds that aren't kept. */
  Pass
  evaluateAll(const Scene &scene, const Library &lib, bool incremental, Status &foreign);
  EvalStatus evaluateOne(const Library &lib,
                         BlockHandle h,
                         OverrideProvider *provider,
                         const TransformType *type,
                         double time,
                         const TransformState &parent,
                         TransformState &out,
                         Status &foreign);
  Box3d boundsOf(Runtime &rt, const SceneMember &m, Spatial &sp, Status &foreign);
  void keepBoxes(const Library &lib);
  void forgetBoxes();
  /** Evaluates those of a level whose types have evaluateMany, in order_[from, to), into
   * batch_. */
  void evaluateLevel(const Library &lib,
                     int from,
                     int to,
                     OverrideProvider *provider,
                     double time,
                     const litestl::util::Vector<ObjectEval> &results,
                     Status &foreign);
  bool stillValid(const Scene &scene, const Library &lib) const;
  /** Sets SceneMember::driven, if the set or the lookups moved. True if it did. */
  bool markDriven(const Library &lib, const OverrideProvider::Driven &driven);

  /* ---- part B: what changed since the last pass (evaluator_dirty.cc) ---- */

  /** The listener's: a notification of a block. */
  void markDirty(const BlockId &id);
  /** After lookUp: the places by block id, nothing dirty, and a full pass next. */
  void indexPlaces(const Library &lib);
  /** Whether the bounds of member i are asked for in every pass. */
  bool boundsEachPass(const SceneMember &m) const
  {
    return m.box >= 0 ? boxStale_[m.box] != 0 : m.data || m.spatial != CapMode::None;
  }
  /** Brings the back store up to the front one, at the places where they differ. */
  void syncBack();
  /** After a pass: what is dirty now is what was notified during it. */
  void clearDirty(const litestl::util::Vector<int> &before);
  /** The provider for member i, or null if it doesn't drive it. */
  OverrideProvider *providerFor(int i, OverrideProvider *provider) const
  {
    return provider && (drivenAll_ || members_[i].driven) ? provider : nullptr;
  }
  void lookUp(const Scene &scene, const Library &lib);
  /** `before` is what was found for it last time, if anything. */
  SceneMember
  memberFor(const Library &lib, BlockHandle h, bool inScene, const SceneMember *before);
  int typeFor(const Library &lib, TypeId type);
  void sortByParent();
  void sortByDepth();

  litestl::util::Vector<SceneBlockType, 4> types_;
  litestl::util::Vector<SceneMember> members_;
  litestl::util::Vector<PrefetchHint> hints_;
  /** The first ones are the scene's members. */
  int inScene_ = 0;
  /** Parents before children. With batches, in the order of their depths. */
  litestl::util::Vector<int> order_;
  bool byDepth_ = false;
  litestl::util::Vector<BatchItem> batch_;
  /** Blocks whose bounds are kept, by id: a change of one makes its box invalid. */
  litestl::util::Map<BlockId, int> boxOf_;
  litestl::util::Vector<KeptBox> boxes_;
  ScopedConnection changeListener_;
  /** Moves when members or parents may be others; and what it was for the last results.
   */
  uint64_t lookups_ = 0;
  uint64_t frontLookups_ = 0;
  /** children(): the handles, and where those of each place begin; the roots are the
   * last. */
  mutable litestl::util::Vector<BlockHandle> children_;
  mutable litestl::util::Vector<int> firstChild_;
  mutable uint64_t childrenOf_ = ~uint64_t(0);
  /** recorded(), at an epoch and a version. A loop over the paths asks for it at each
   * step. */
  mutable litestl::util::Vector<BlockHandle> recorded_;
  mutable uint64_t recordedEpoch_ = 0;
  mutable uint64_t recordedVersion_ = 0;
  /** 1 for the members of a cycle. */
  litestl::util::Vector<uint8_t> cyclic_;
  std::shared_ptr<const HandleIndex> index_;
  LookupKey key_;
  bool valid_ = false;
  /* What markDriven last saw. */
  bool drivenAll_ = true;
  uint64_t drivenVersion_ = 0;
  uint64_t drivenLookups_ = ~uint64_t(0);
  ChannelBlock scratch_;
  ChannelOverrides overrides_;
  /** The places that differ, of the pass that runs. */
  litestl::util::Vector<int32_t> differing_;
  EvaluatedStore stores_[2];
  int front_ = 0;

  /* Part B. */
  litestl::util::Map<BlockId, int> placeOf_;
  litestl::util::Vector<uint8_t> dirty_;
  litestl::util::Vector<int> dirtyList_;
  /** The places whose state differs in this pass, for their children. */
  litestl::util::Vector<uint8_t> moved_;
  litestl::util::Vector<int> movedList_;
  /** The next pass evaluates everything. */
  bool full_ = true;
  double lastTime_ = 0.0;
  /** Where the back store differs from the front one; all of it if backAll_. */
  litestl::util::Vector<int32_t> backDiff_;
  bool backAll_ = true;
  /** Kept boxes that weren't valid when the pass began: all who share one ask again. */
  litestl::util::Vector<uint8_t> boxStale_;
  /** Each member's Spatial, when it is a table's: it is kept until the lookups move. */
  litestl::util::Vector<Spatial> spatial_;
  litestl::util::Vector<uint8_t> spatialKept_;
  /** The members that use a data block: the first, then each one's next, or -1. */
  litestl::util::Map<BlockId, int> dataUsers_;
  litestl::util::Vector<int> nextUser_;
};

} // namespace coredcc::detail

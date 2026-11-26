#pragma once

#include "coredcc/api/context.h"
#include "coredcc/lib/block_io.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/overrides.h"
#include "coredcc/scene/transform_registry.h"

#include <memory>
#include <span>
#include <utility>

namespace coredcc {

namespace detail {

/** Nothing, or a T of its own: copies have copies. */
template <class T> class OwnCopy {
public:
  OwnCopy() = default;
  OwnCopy(const OwnCopy &b) : p_(b.p_ ? std::make_unique<T>(*b.p_) : nullptr)
  {
  }
  OwnCopy(OwnCopy &&) noexcept = default;
  OwnCopy &operator=(OwnCopy b) noexcept
  {
    p_ = std::move(b.p_);
    return *this;
  }
  void make()
  {
    p_ = std::make_unique<T>();
  }
  void reset()
  {
    p_.reset();
  }
  explicit operator bool() const
  {
    return p_ != nullptr;
  }
  T *operator->() const
  {
    return p_.get();
  }
  T *get() const
  {
    return p_.get();
  }

private:
  std::unique_ptr<T> p_;
};

} // namespace detail

/** coredcc's own object: a block of type kObjectType. */
inline constexpr const char *kObjectType = "coredcc.Object";
/** An empty struct. Blocks whose structs have it as their parent can be set as an
 * object's `data` through the datapath. */
inline constexpr const char *kObjectDataStruct = "coredcc.ObjectData";

/** What a Transformable block has by path: `name`, `flags`, `parent`, `data`,
 * `transform`, `transformType` and `locks`, through its Transformable. Blocks whose
 * structs have it as their parent, coredcc.Object among them, can be set as a parent or
 * an active object through the datapath, and are what a scene's collections have. Setting
 * what the Transformable has no slot for is Unsupported. */
inline constexpr const char *kSceneObjectStruct = "coredcc.SceneObject";

/** The data of a `coredcc.Object`. Implements Transformable. */
class Object {
  struct FileData;
  /* First, with `flags`: evaluating reads them, and the members up to `channels`, of
   * every object. */
  TypeId type_;
  uint64_t locks_ = 0;
  detail::OwnCopy<FileData> file_;

public:
  ObjectFlags flags = ObjectFlags::None;
  /** Kept alive by its children, also when it is in no scene. Deleting it doesn't delete
   * them. */
  OptRef<Object> parent;
  OptRef<void> data;
  /** Empty for an object of a file until its transform type is registered: read() has
   * them, and the locks. */
  ChannelBlock channels;

  /** The transform type's name, kept also while no such type is registered. */
  const litestl::util::string &transformType() const
  {
    return typeName_;
  }
  TypeId transformTypeId() const
  {
    return type_;
  }
  /** Any name. The Runtime learns it, so setTransformTypeId can come back to it. */
  void setTransformType(std::string_view name);

  /* Transformable. */
  void read(ObjectInfo &info, ChannelBlock &out) const;
  Status writeChannels(const ChannelBlock &in);
  Status setParentId(const BlockId &id);
  Status setTransformTypeId(const TypeId &type);
  Status setFlags(uint32_t f);
  Status setLocks(uint64_t locks);

  /* Files. What this build doesn't know is kept, and written again: entries of a newer
   * build, channels of a transform type that isn't there or is of another version. The
   * names `dataMatrix` (an offset of the data that children don't have) and `parentKey`
   * (a part of the parent) are reserved. */

  void write(BlockWriter &out) const;
  /** Fails the reader for a payload that is wrong. */
  void readFrom(BlockReader &in);
  /** Reads the file's channels, if the transform type is there and can
   * (TransformModel::upgrade). An error is sticky. */
  void resolve();
  /** What a file has for the object, besides its channels, that this build doesn't know.
   */
  std::span<const BlockEntry> unreadFields() const
  {
    return file_ ? std::span<const BlockEntry>(file_->fields.data(),
                                               size_t(file_->fields.size()))
                 : std::span<const BlockEntry>();
  }
  /** What a file has for this transform type that no channel takes, or that waits for the
   * type. */
  std::span<const BlockEntry> unread() const
  {
    const FileData *f = fileOfType();
    return f ? std::span<const BlockEntry>(f->unread.data(), size_t(f->unread.size()))
             : std::span<const BlockEntry>();
  }
  /** Every reference, for the provider. */
  template <class Fn> void forEachCell(Fn &&fn)
  {
    fn(parent.cell());
    fn(data.cell());
    if (file_) {
      forEachCellOf(file_->fields, fn);
    }
    if (fileOfType()) {
      forEachCellOf(file_->unread, fn);
    }
  }
  uint64_t byteSize() const;

  explicit Object(Runtime *rt) : rt_(rt)
  {
  }
  static litestl::binding::types::Struct<Object> *defineBindings();

private:
  template <class Fn>
  static void forEachCellOf(litestl::util::Vector<BlockEntry> &entries, Fn &&fn)
  {
    for (BlockEntry &e : entries) {
      for (RefCell &c : e.refs) {
        fn(c);
      }
    }
  }
  /* What few objects have. In the object itself it took 1.8 KB of each one, and
   * evaluating many was slower by half. */
  struct FileData {
    litestl::util::Vector<BlockEntry> fields;

    /* The transform type that the rest is of. After a change to another type it is kept
     * for an undo, but isn't the object's. */
    litestl::util::string type;
    litestl::util::Vector<BlockEntry> unread;
    /* Names of locked channels that this build doesn't know. */
    litestl::util::string locks;
    uint32_t version = 0;
    /* `unread` is all of the type's channels, not read yet. */
    bool pending = false;
    bool upgradeFailed = false;
  };
  /* If there is something of the type that the object has now. */
  const FileData *fileOfType() const
  {
    return file_ && file_->type == typeName_ ? file_.get() : nullptr;
  }
  bool upgrade(const TransformType &type, litestl::util::Vector<BlockEntry> &entries);
  void takeLocks(const TransformType *type);
  void dropFileData(bool versionKnown);

  Runtime *rt_;
  litestl::util::string typeName_;
};

/** A new Object with a transform type's defaults. */
Result<BlockHandle>
newObject(Library &lib, std::string_view name, std::string_view transformType);

/** How an object's evaluation went. */
enum class EvalStatus : int32_t {
  Ok = 0,
  /** Its transform isn't finite, or its channels are another type's. It is where its
     parent is. */
  Invalid = 1,
  /** Its transform model, its Transformable, or what drives it failed. It is where its
     parent is. */
  ModelFailed = 2,
  /** Its transform type isn't registered. It is where its parent is. */
  MissingModel = 3,
  /** It is its own ancestor. It has no parent until that changes. */
  Cycle = 4,
  /** It isn't Transformable. It is at the origin. */
  NoTransform = 5,
};

/** A model's answer, with what stands in for a bad one. `error` gets the first exception.
 */
EvalStatus evaluateObject(Runtime &rt,
                          const TransformType *type,
                          const EvalContext &ctx,
                          const TransformState &parent,
                          TransformState &out,
                          Status &error);

/* What commands use: computed from the objects themselves, and what drives them, never
 * from the last evaluation. They are Busy while a scene or a graph evaluates, and fail
 * while an error is pending. A parent that isn't Live counts as none. `time` is in
 * seconds. */

/** The state of an object. */
Result<TransformState> computeState(const Library &lib, BlockHandle object, double time);
/** The state of an object's parent: the identity without one. */
Result<TransformState>
computeParentState(const Library &lib, BlockHandle object, double time);
/** The point that an object turns about, in world space: where a gizmo for it belongs. */
Result<double3> computePivot(const Library &lib, BlockHandle object, double time);
/** Channels that put an object at `target` (see solveWorld). Writes nothing. Locked
 * channels stay. */
Result<ChannelBlock> setWorldOf(const Library &lib,
                                BlockHandle object,
                                const mat4d &target,
                                SetWorldMode mode,
                                double time);
/** Whether `ancestor` is `object` or above it. */
bool isSelfOrAncestor(const Library &lib, BlockHandle ancestor, BlockHandle object);

/** An object's Transformable, read. */
struct ObjectRead {
  Transformable iface;
  ObjectInfo info;
  ChannelBlock channels;
  /** Null if it isn't registered. */
  const TransformType *type = nullptr;
  OwnerKey owner;
  /* Of readObjectAt. */
  ChannelOverrides overrides;
  double time = 0.0;

  /** What its model is given. */
  EvalContext ctx() const
  {
    return overrides.any() ? EvalContext(channels, time, overrides.values, overrides.mask)
                           : EvalContext(channels, time);
  }
  /** What a tool that moves it gives to a solver: the same, with its locks. */
  EvalContext solveContext() const
  {
    return ctx().withLocks(info.locks);
  }
};
Result<ObjectRead> readObject(const Library &lib, BlockHandle object);
/** The same, with what drives its channels at `time` (OverrideProvider). */
Result<ObjectRead> readObjectAt(const Library &lib, BlockHandle object, double time);

/** Stores an object's channels, and posts a change of its `transform`. */
Status writeTransform(Context &ctx, BlockHandle object, const ChannelBlock &channels);
/** Stores another transform type and its channels. */
Status writeTransformType(Context &ctx,
                          BlockHandle object,
                          TypeId type,
                          const ChannelBlock &channels);
/** Stores an object's flags, and posts a change of its `flags`. Unsupported for one that
 * can't. */
Status writeFlags(Context &ctx, BlockHandle object, ObjectFlags flags);
/** Stores which of an object's channels are locked, and posts a change of its `locks`. */
Status writeLocks(Context &ctx, BlockHandle object, uint64_t locks);
/** Stores an object's parent (null for none), and posts a change of its `parent`. Doesn't
 * look for cycles. */
Status writeParent(Context &ctx, BlockHandle object, const BlockId &parent);

/** Registers the scene's types (idempotent): the transform types, the capabilities, the
 * blocks coredcc.Object and coredcc.Scene, their structs, and the owner of the results.
 * When one of the names is taken it fails, and again each time. */
Status registerSceneTypes(Runtime &rt);

namespace detail {
Status registerObjectType(Runtime &rt);
Status registerSceneType(Runtime &rt);
Status registerEvaluated(Runtime &rt);
/** Whether that was done: the last thing registerSceneTypes does. */
bool evaluatedRegistered(Runtime &rt);
/** The struct of coredcc.SceneObject: what any Transformable block has by path. */
ApiStruct sceneObjectStruct();
/** The struct of coredcc.Object. */
ApiStruct objectStruct();
/** The Live block an id names, or null. */
BlockHandle liveBlock(const Library &lib, const BlockId &id);
/** The root of the block that `cell` names, or a null PointerRef unless it is Live. */
PointerRef blockRoot(Context &ctx, BlockHandle h);
} // namespace detail

} // namespace coredcc

namespace litestl::binding {
/** EvalStatus, bound as an enum (scene/bindings.cc). */
template <> struct Binder<coredcc::EvalStatus> {
  static const BindingBase *bind();
};
} // namespace litestl::binding

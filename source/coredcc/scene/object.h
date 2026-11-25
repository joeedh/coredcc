#pragma once

#include "coredcc/api/context.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/transform_registry.h"

namespace coredcc {

/** coredcc's own object: a block of type kObjectType. */
inline constexpr const char *kObjectType = "coredcc.Object";
/** An empty struct. Blocks whose structs have it as their parent can be set as an object's
 * `data` through the datapath. */
inline constexpr const char *kObjectDataStruct = "coredcc.ObjectData";

/** An empty struct. Blocks whose structs have it as their parent, coredcc.Object among them, can be
 * set as a parent or an active object through the datapath. */
inline constexpr const char *kSceneObjectStruct = "coredcc.SceneObject";

/** The data of a `coredcc.Object`. Implements Transformable. */
class Object {
 public:
  ObjectFlags flags = ObjectFlags::None;
  WeakRef<Object> parent;
  OptRef<void> data;
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

  explicit Object(Runtime *rt) : rt_(rt) {}
  static litestl::binding::types::Struct<Object> *defineBindings();

 private:
  Runtime *rt_;
  litestl::util::string typeName_;
  TypeId type_;
};

/** A new Object with a transform type's defaults. */
Result<BlockHandle> newObject(Library &lib, std::string_view name, std::string_view transformType);

/** How an object's evaluation went. */
enum class EvalStatus : int32_t {
  Ok = 0,
  /** Its transform isn't finite, or its channels are another type's. It is where its parent is. */
  Invalid = 1,
  /** Its transform model, or its Transformable, threw. It is where its parent is. */
  ModelFailed = 2,
  /** Its transform type isn't registered. It is where its parent is. */
  MissingModel = 3,
  /** It is its own ancestor. It has no parent until that changes. */
  Cycle = 4,
  /** It isn't Transformable. It is at the origin. */
  NoTransform = 5,
};

/** A model's answer, with what stands in for a bad one. `error` gets the first exception. */
EvalStatus evaluateObject(Runtime &rt,
                          const TransformType *type,
                          const ChannelBlock &channels,
                          double time,
                          const TransformState &parent,
                          TransformState &out,
                          Status &error);

/* What commands use: computed from the objects themselves, never from the last evaluation. They
 * are Busy while a scene or a graph evaluates, and fail while an error is pending. A parent
 * that isn't Live counts as none. `time` is in seconds. */

/** The state of an object. */
Result<TransformState> computeState(const Library &lib, BlockHandle object, double time);
/** The state of an object's parent: the identity without one. */
Result<TransformState> computeParentState(const Library &lib, BlockHandle object, double time);
/** Channels that put an object at `target` (see solveWorld). Writes nothing. */
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
};
Result<ObjectRead> readObject(const Library &lib, BlockHandle object);

/** Stores an object's channels, and posts a change of its `transform`. */
Status writeTransform(Context &ctx, BlockHandle object, const ChannelBlock &channels);
/** Stores another transform type and its channels. */
Status writeTransformType(Context &ctx,
                          BlockHandle object,
                          TypeId type,
                          const ChannelBlock &channels);
/** Stores an object's parent (null for none), and posts a change of its `parent`. Doesn't look
 * for cycles. */
Status writeParent(Context &ctx, BlockHandle object, const BlockId &parent);

/** Registers the scene's types (idempotent): the transform types, the capabilities, the
 * blocks coredcc.Object and coredcc.Scene, their structs, and the owner of the results. */
Status registerSceneTypes(Runtime &rt);

namespace detail {
Status registerObjectType(Runtime &rt);
Status registerSceneType(Runtime &rt);
Status registerEvaluated(Runtime &rt);
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

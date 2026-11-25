#pragma once

#include "coredcc/api/struct.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/runtime.h"
#include "coredcc/scene/transform.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/** A transform type as it is registered. */
struct TransformTypeInfo {
  /** Unique, e.g. "core.transform.trs". Names starting with "core." are coredcc's. */
  litestl::util::string name;
  litestl::util::string uiName;
  litestl::util::string description;
  /**
   * Bool, Int, Float, Vec2 to Vec4, Enum and Quat; not dynamic. Their components are the
   * channels, in order, and their defaults the initial values. Lengths that move the object must
   * have subtype Distance: solveWorld's tolerance comes from their size.
   */
  litestl::util::Vector<PropertyDef, 1> channels;
};

/** A registered transform type. Lives as long as its Runtime. */
class TransformType {
 public:
  TypeId id() const
  {
    return id_;
  }
  const litestl::util::string &name() const
  {
    return name_;
  }
  /** Its channels as a struct, over a ChannelBlock. */
  StructId structId() const
  {
    return struct_;
  }
  int32_t channelCount() const
  {
    return defaults_.size();
  }
  const ChannelBlock &defaults() const
  {
    return defaults_;
  }
  /** Whether channel i belongs to a def of subtype Distance. */
  bool isDistance(int32_t i) const
  {
    return i >= 0 && i < int32_t(distance_.size()) && distance_[i] != 0;
  }
  const TransformModel &model() const
  {
    return model_.get();
  }
  Runtime &runtime() const
  {
    return *rt_;
  }

 private:
  friend class TransformRegistry;
  TypeId id_;
  litestl::util::string name_;
  StructId struct_;
  ChannelBlock defaults_;
  litestl::util::Vector<uint8_t> distance_;
  Owned<TransformModel> model_;
  Runtime *rt_ = nullptr;
};

/**
 * Transform types by name. A Runtime extension.
 *
 * Adding one registers an ApiStruct with the type's name, whose parent is `kChannelsStruct`.
 * Its data is a ChannelBlock. Setting an Int outside +-2^53 is OutOfRange (a double can't hold
 * it); reads round like EvalCtx, and give an Enum's first item for a value that isn't one.
 */
class TransformRegistry {
 public:
  static constexpr const char *kExtName = "coredcc.TransformRegistry";
  static constexpr const char *kChannelsStruct = "coredcc.TransformChannels";

  explicit TransformRegistry(Runtime *rt);
  ~TransformRegistry();
  TransformRegistry(const TransformRegistry &) = delete;
  TransformRegistry &operator=(const TransformRegistry &) = delete;

  /** Registers a type and takes its model. A name that is taken is AlreadyExists. */
  Result<const TransformType *> add(TransformTypeInfo info, Owned<TransformModel> model);
  const TransformType *find(TypeId id) const;
  const TransformType *find(std::string_view name) const
  {
    return find(TypeId::of(name));
  }
  /** In registration order. */
  int32_t count() const
  {
    return int32_t(types_.size());
  }
  const TransformType &at(int32_t i) const
  {
    return *types_[i];
  }

 private:
  Runtime &rt_;
  litestl::util::Vector<TransformType *> types_;
  litestl::util::Map<uint64_t, TransformType *> byId_;
};

/* The checked ways to call a model. All are FailedPrecondition while the Runtime has a sticky
 * error, and for a ChannelBlock of the wrong size; a foreign model's exception is returned, not
 * left as the sticky error. */

/** The model's state, which needn't be finite. */
Result<TransformState> evaluateTransform(const TransformType &type,
                                         const EvalCtx &ctx,
                                         const TransformState &parent);

/**
 * Channels that put the object at `target`, an affine matrix.
 *
 * - `current` comes back unchanged, without asking the model, if it already gives the target.
 *   So an object's own world is always found.
 * - Exact: otherwise the model's answer is evaluated and compared: each column of the linear
 *   part within 1e-9 of its length (which refuses float matrices), and the translation within
 *   16 epsilon of the magnitudes that went into it. Beyond that it is FailedPrecondition. So are
 *   some targets under parents scaled very unequally at several levels, or along one axis by
 *   1e-7: use Closest.
 * - Closest: only the translation is compared; shear is lost.
 * - A parent without an inverse is FailedPrecondition in both modes, and so is a result that
 *   isn't finite. A target with a scale of 0 is fine.
 */
Result<ChannelBlock> solveWorld(const TransformType &type,
                                const EvalCtx &current,
                                const mat4d &target,
                                const TransformState &parent,
                                SetWorldMode mode);

/** Applies `delta`, the total since `base`. `nearTo` is the previous result of the interaction,
 * or base's channels: Euler angles stay continuous with it. */
Result<ChannelBlock> solveDelta(const TransformType &type,
                                const EvalCtx &base,
                                const ChannelBlock &nearTo,
                                const TransformDelta &delta,
                                const TransformState &parent);

} // namespace coredcc

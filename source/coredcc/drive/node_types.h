#pragma once

/** \file
 * The node types that connect graphs with a scene's objects (internal to drive/, and for
 * tests): the transform state as a payload, users' adapters, and the transform nodes that
 * SceneDrivers generates.
 */

#include "coredcc/graph/node_type.h"
#include "coredcc/graph/payload.h"
#include "coredcc/scene/transform.h"

#include <cstring>
#include <string_view>

namespace coredcc {

template <> struct PayloadTraits<TransformState> {
  static constexpr const char *name = "coredcc.TransformState";
  /** Bitwise, so that a state that is computed again the same stops what follows. */
  static bool equal(const TransformState &a, const TransformState &b)
  {
    return memcmp(&a, &b, sizeof(TransformState)) == 0;
  }
};

class TransformType;

namespace drive {

inline constexpr const char *kStatePayload = "coredcc.TransformState";
inline constexpr const char *kTimeNode = "scene.time";
inline constexpr const char *kStateNode = "scene.object.state";
/** `scene.drive.<kind>`, for the kinds below. */
inline constexpr const char *kDrivePrefix = "scene.drive.";
/** `scene.transform.<transform type>`, and the one for a type that isn't registered. */
inline constexpr const char *kTransformPrefix = "scene.transform.";
/** Not under kTransformPrefix, so that no transform type's name can give it. */
inline constexpr const char *kMissingTransform = "scene.missingTransform";
/** The struct that `object` params point to. */
inline constexpr const char *kObjectStruct = "coredcc.SceneObject";

/** What a drive adapter drives: a channel def's kind, as a socket's. */
enum class DriveKind : uint8_t { Bool, Int, Float, Vec2, Vec3, Vec4, Quat, None };

/** The kind of a channel def (an Enum is an Int); None for one that can't be driven. */
DriveKind kindOf(PropKind kind);
/** The kind of a `scene.drive.<kind>` type name; None for another name. */
DriveKind kindOfDriveType(std::string_view typeName);
/** "bool", "int", ...: the socket type, and the end of the adapter's type name. */
const char *kindName(DriveKind kind);
/** How many channels a value of the kind fills. */
int kindChannels(DriveKind kind);
/** Writes a value of the kind into `out[0, kindChannels)`. False if it isn't of it. */
bool flatten(const GraphValue &v, DriveKind kind, double *out);

/** Inputs of transform nodes: `parent`, `time`, then one per channel def. */
inline constexpr int kParentInput = 0;
inline constexpr int kTimeInput = 1;
inline constexpr int kFirstChannelInput = 2;
/** The input of a channel def: its name with this in front, so that no name collides. */
inline constexpr const char *kChannelPrefix = "c_";
/** A transform node's `status` when the object isn't what the node was made for (another
 * parent or transform type): the graph is out of date. */
inline constexpr int64_t kStaleStatus = -1;

/** Registers the payload type, scene.time, scene.object.state, scene.drive.*, and the
 * transform node for a missing type. Once per Runtime; AlreadyExists after. */
Status registerNodeTypes(Runtime &rt);
/** The transform node type of a transform type, registered when first asked for. Null
 * for null, which is kMissingTransform. AlreadyExists if a type of that name is someone
 * else's. Users can place these in a graph; the scan of listed graphs reports them. */
Result<const NodeType *> transformNodeType(Runtime &rt, const TransformType *type);
/** How many transform node types drive/ registered, the missing type's included. */
int ownTransformTypeCount(Runtime &rt);

} // namespace drive
} // namespace coredcc

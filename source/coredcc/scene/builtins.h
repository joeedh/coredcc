#pragma once

#include "coredcc/base/status.h"
#include "coredcc/scene/transform.h"

namespace coredcc {

class Runtime;

/** Location, rotation (Euler or quaternion) and scale. */
inline constexpr const char *kTrsTransform = "core.transform.trs";
/** FBX's node transform: pivots, pre- and post-rotation and inherit types. */
inline constexpr const char *kFbxTransform = "core.transform.fbx";
/** Any affine matrix. */
inline constexpr const char *kMatrixTransform = "core.transform.matrix";

/** Registers the built-in transform types, unless they are registered. */
Status registerTransformTypes(Runtime &rt);

/* The index of each channel's first component. Angles are in radians. */

namespace trs {
enum Channel : int32_t {
  kLocation = 0,
  /** An EulerOrder, or kQuaternionMode. Setting it converts nothing. */
  kRotationMode = 3,
  kRotationEuler = 4,
  /** x, y, z, w; needn't be unit length. Zero and non-finite ones are the identity. */
  kRotationQuaternion = 7,
  kScale = 11,
  kCount = 14,
};
inline constexpr int32_t kQuaternionMode = 6;
} // namespace trs

/**
 * Local is `T Roff Rp Rpre R Rpost^-1 Rp^-1 Soff Sp S Sp^-1`. Pre- and post-rotation are
 * always of order XYZ. In an Rrs node, a parent scale of 0 counts as 1 when it is divided
 * out.
 */
namespace fbx {
enum Channel : int32_t {
  kTranslation = 0,
  kRotation = 3,
  /** An EulerOrder: not FBX's numbers, which swap YXZ and YZX. */
  kRotationOrder = 6,
  kPreRotation = 7,
  kPostRotation = 10,
  kRotationOffset = 13,
  kRotationPivot = 16,
  kScalingOffset = 19,
  kScalingPivot = 22,
  kScaling = 25,
  kInheritType = 28,
  kCount = 29,
};
/** How the parent's scale reaches the node, with FBX's numbers. `R` and `r` are the
 * parent's and the node's rotation, `S` and `s` the scales, in the order they apply in.
 */
enum InheritType : int32_t { kRrSs = 0, kRSrs = 1, kRrs = 2 };
} // namespace fbx

/** The columns of an affine matrix. Its state's rotation and scale are those of
 * decomposeTrs, but for rounding. */
namespace matrix_transform {
enum Channel : int32_t { kColumns = 0, kTranslation = 9, kCount = 12 };
} // namespace matrix_transform

} // namespace coredcc

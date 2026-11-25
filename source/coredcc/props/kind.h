#pragma once

/** \file
 * What a property is: its kind, subtype, access flags and UI hints (architecture §4).
 */

#include "coredcc/base/flags.h"

#include <cstdint>

namespace coredcc {

/** The value kind of a property. */
enum class PropKind : uint8_t {
  Bool,
  Int,
  Float,
  String,
  Enum,
  Flags,
  Vec2,
  Vec3,
  Vec4,
  Mat4,
  Quat,
  Pointer,
  Collection,
};

/** Stable name of a kind, e.g. "Float". */
const char *propKindName(PropKind kind);

/** Int and Float. */
constexpr bool isNumeric(PropKind k)
{
  return k == PropKind::Int || k == PropKind::Float;
}

/** Vec2, Vec3 and Vec4. */
constexpr bool isVector(PropKind k)
{
  return k == PropKind::Vec2 || k == PropKind::Vec3 || k == PropKind::Vec4;
}

/** Number of scalar components: 1 for scalars, 2-4 for vectors and Quat, 16 for Mat4,
 * and 0 for String, Pointer and Collection. */
constexpr int componentCount(PropKind k)
{
  switch (k) {
    case PropKind::Bool:
    case PropKind::Int:
    case PropKind::Float:
    case PropKind::Enum:
    case PropKind::Flags:
      return 1;
    case PropKind::Vec2:
      return 2;
    case PropKind::Vec3:
      return 3;
    case PropKind::Vec4:
    case PropKind::Quat:
      return 4;
    case PropKind::Mat4:
      return 16;
    case PropKind::String:
    case PropKind::Pointer:
    case PropKind::Collection:
      return 0;
  }
  return 0;
}

/** What a value means, for display and editing. It never changes how it is stored. */
enum class PropSubtype : uint8_t {
  None,
  Color,
  ColorGamma,
  Angle,
  Distance,
  Factor,
  Percentage,
  Time,
  Direction,
  Euler,
  Xyz,
  FilePath,
  Password,
};

const char *propSubtypeName(PropSubtype subtype);

enum class PropAccess : uint16_t {
  None = 0,
  ReadOnly = 1 << 0,
  /** Hidden from UI and bindings listings; still resolvable by path. */
  Private = 1 << 1,
  /** Sets don't create undo steps. */
  NoUndo = 1 << 2,
  /** May be unset (a command input without a default). */
  Optional = 1 << 3,
  /** A command input remembered between runs (LastUsedStore). */
  SaveLastValue = 1 << 4,
  Animatable = 1 << 5,
  /** A command input LastUsedStore doesn't keep, such as block ids as text: a later run without
   * it must not get the old value. */
  NotRemembered = 1 << 6,
};
CDCC_FLAG_OPERATORS(PropAccess)

enum class SliderStyle : uint8_t { Default, Slider, Number };

/** UI hints. They never affect behaviour. */
struct PropUi {
  bool iconsOnly = false;
  bool expand = false;
  SliderStyle slider = SliderStyle::Default;
  bool multiline = false;
};

} // namespace coredcc

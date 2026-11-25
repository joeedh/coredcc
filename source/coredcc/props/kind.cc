#include "coredcc/props/kind.h"

namespace coredcc {

const char *propKindName(PropKind kind)
{
  switch (kind) {
  case PropKind::Bool:
    return "Bool";
  case PropKind::Int:
    return "Int";
  case PropKind::Float:
    return "Float";
  case PropKind::String:
    return "String";
  case PropKind::Enum:
    return "Enum";
  case PropKind::Flags:
    return "Flags";
  case PropKind::Vec2:
    return "Vec2";
  case PropKind::Vec3:
    return "Vec3";
  case PropKind::Vec4:
    return "Vec4";
  case PropKind::Mat4:
    return "Mat4";
  case PropKind::Quat:
    return "Quat";
  case PropKind::Pointer:
    return "Pointer";
  case PropKind::Collection:
    return "Collection";
  }
  return "?";
}

const char *propSubtypeName(PropSubtype subtype)
{
  switch (subtype) {
  case PropSubtype::None:
    return "None";
  case PropSubtype::Color:
    return "Color";
  case PropSubtype::ColorGamma:
    return "ColorGamma";
  case PropSubtype::Angle:
    return "Angle";
  case PropSubtype::Distance:
    return "Distance";
  case PropSubtype::Factor:
    return "Factor";
  case PropSubtype::Percentage:
    return "Percentage";
  case PropSubtype::Time:
    return "Time";
  case PropSubtype::Direction:
    return "Direction";
  case PropSubtype::Euler:
    return "Euler";
  case PropSubtype::Xyz:
    return "Xyz";
  case PropSubtype::FilePath:
    return "FilePath";
  case PropSubtype::Password:
    return "Password";
  }
  return "?";
}

} // namespace coredcc

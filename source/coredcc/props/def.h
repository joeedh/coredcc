#pragma once

/** \file
 * PropertyDef: the description of one property, as structs, commands and tools
 * declare them (architecture §4), and coerce(), the single place values are converted
 * to a property's kind.
 */

#include "coredcc/base/id.h"
#include "coredcc/base/status.h"
#include "coredcc/props/kind.h"
#include "coredcc/props/units.h"
#include "coredcc/props/value.h"

#include "litestl/util/function.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <limits>
#include <string_view>
#include <variant>

namespace coredcc {

struct EnumItem {
  /** The identifier scripts and files use, e.g. "SMOOTH". */
  litestl::util::string key;
  int64_t value = 0;
  litestl::util::string uiName;
  litestl::util::string description;
  litestl::util::string icon;
};

/** Int, Float, vector, Quat and Mat4 properties. Ranges apply to each component. */
struct NumericMeta {
  /** coerce() clamps to the hard range. */
  double hardMin = -std::numeric_limits<double>::infinity();
  double hardMax = std::numeric_limits<double>::infinity();
  /** The UI's slider range, inside the hard range. An infinite bound means the hard
   * range's bound. */
  double softMin = -std::numeric_limits<double>::infinity();
  double softMax = std::numeric_limits<double>::infinity();
  /** UI drag step; 0 means a default. */
  double step = 0.0;
  /** Decimals shown, 0 to 15. */
  int precision = 3;
  UnitFamily unit = UnitFamily::None;
  /** What to show values in: a unit of the family (units::findUnit), or empty for its base unit.
   * Values are in the base unit all the same. */
  litestl::util::string displayUnit;

  /** displayUnit's symbol, or the base unit's. Empty for a family without units. */
  const char *displaySymbol() const;
};

/** The name of component `i` for a UI: "X", "Y", "Z" and "W" of vectors and quaternions, "R", "G",
 * "B" and "A" of colors. Null for others, as a Mat4's. */
const char *componentName(PropKind kind, PropSubtype subtype, int i);

/** Enum and Flags properties. Flags items are single bits. */
struct EnumMeta {
  litestl::util::Vector<EnumItem, 1> items;

  const EnumItem *findKey(std::string_view key) const;
  const EnumItem *findValue(int64_t value) const;
};

struct StringMeta {
  /** 0 for no limit. */
  int64_t maxLength = 0;
};

/** Pointer and Collection properties: the struct they point to or hold. */
struct TargetMeta {
  StructId target;
};

class PropertyDef;

/** Patches a copy of a def at read time, for owner-dependent ranges, items or flags.
 * `owner` is the struct instance the property belongs to (a PropertyBag for bag
 * entries); never null: validate() checks the static def without calling the hook. */
using PropDynamicFn = litestl::util::function<void(PropertyDef &def, const void *owner)>;

/**
 * A property's description. A value type, built fluently:
 *
 *     PropertyDef::floatProp("radius").range(0, 100).softRange(0, 10)
 *         .subtype(PropSubtype::Distance).unit(UnitFamily::Length)
 *         .defaultValue(PropValue::ofFloat(1.0));
 *
 * The kind-specific metadata is read through numeric(), enumMeta(), stringMeta() and
 * target(); a builder call for metadata the kind doesn't have aborts (a programmer
 * error). validate() checks the whole def, and registries call it.
 */
class PropertyDef {
 public:
  PropertyDef(PropKind kind, std::string_view apiName);

  static PropertyDef boolProp(std::string_view name)
  {
    return {PropKind::Bool, name};
  }
  static PropertyDef intProp(std::string_view name)
  {
    return {PropKind::Int, name};
  }
  static PropertyDef floatProp(std::string_view name)
  {
    return {PropKind::Float, name};
  }
  static PropertyDef stringProp(std::string_view name)
  {
    return {PropKind::String, name};
  }
  static PropertyDef enumProp(std::string_view name)
  {
    return {PropKind::Enum, name};
  }
  static PropertyDef flagsProp(std::string_view name)
  {
    return {PropKind::Flags, name};
  }
  static PropertyDef vecProp(std::string_view name, int components);
  static PropertyDef mat4Prop(std::string_view name)
  {
    return {PropKind::Mat4, name};
  }
  static PropertyDef quatProp(std::string_view name)
  {
    return {PropKind::Quat, name};
  }
  static PropertyDef pointerProp(std::string_view name, StructId target);
  static PropertyDef collectionProp(std::string_view name, StructId element);

  /* ---- builder ---- */
  PropertyDef &uiName(std::string_view s);
  PropertyDef &description(std::string_view s);
  PropertyDef &icon(std::string_view s);
  PropertyDef &subtype(PropSubtype s);
  /** Adds access flags (the other builder calls set). */
  PropertyDef &access(PropAccess a);
  PropertyDef &ui(PropUi u);
  PropertyDef &defaultValue(PropValue v);
  PropertyDef &dynamic(PropDynamicFn fn);
  /* Numeric kinds only. */
  PropertyDef &range(double hardMin, double hardMax);
  PropertyDef &softRange(double softMin, double softMax);
  PropertyDef &step(double s);
  PropertyDef &precision(int p);
  /** `display` as NumericMeta::displayUnit. */
  PropertyDef &unit(UnitFamily u, std::string_view display = {});
  /* Enum and Flags only. */
  PropertyDef &item(std::string_view key,
                    int64_t value,
                    std::string_view uiName = {},
                    std::string_view description = {},
                    std::string_view icon = {});
  /* String only. */
  PropertyDef &maxLength(int64_t n);

  /* ---- reading ---- */
  PropKind kind() const
  {
    return kind_;
  }
  const litestl::util::string &apiName() const
  {
    return apiName_;
  }
  /** The UI name: set explicitly, or derived from apiName ("edgeCrease" -> "Edge
   * Crease"). */
  const litestl::util::string &uiName() const
  {
    return uiName_;
  }
  const litestl::util::string &description() const
  {
    return description_;
  }
  const litestl::util::string &icon() const
  {
    return icon_;
  }
  PropSubtype subtype() const
  {
    return subtype_;
  }
  PropAccess access() const
  {
    return access_;
  }
  bool hasAccess(PropAccess a) const
  {
    return hasFlag(access_, a);
  }
  const PropUi &ui() const
  {
    return ui_;
  }
  const PropValue &defaultValue() const
  {
    return default_;
  }

  const NumericMeta *numeric() const
  {
    return std::get_if<NumericMeta>(&meta_);
  }
  const EnumMeta *enumMeta() const
  {
    return std::get_if<EnumMeta>(&meta_);
  }
  const StringMeta *stringMeta() const
  {
    return std::get_if<StringMeta>(&meta_);
  }
  const TargetMeta *target() const
  {
    return std::get_if<TargetMeta>(&meta_);
  }

  bool isDynamic() const
  {
    return bool(dynamic_);
  }
  /** This def as seen for `owner`: a copy patched by the dynamic hook, if any. */
  PropertyDef resolved(const void *owner) const;

  /** Whether `b` says the same, hooks aside: what a UI shows of them doesn't differ. */
  bool sameAs(const PropertyDef &b) const;

  /**
   * Checks the def: an identifier apiName, ordered ranges with the soft range inside
   * the hard one, precision 0-15, enum items with unique identifier keys and unique
   * values (single bits for Flags, at least one item), and a default value that
   * coerce() accepts unchanged.
   */
  Status validate() const;

 private:
  PropKind kind_;
  litestl::util::string apiName_;
  litestl::util::string uiName_;
  litestl::util::string description_;
  litestl::util::string icon_;
  PropSubtype subtype_ = PropSubtype::None;
  PropAccess access_ = PropAccess::None;
  PropUi ui_;
  PropValue default_;
  std::variant<std::monostate, NumericMeta, EnumMeta, StringMeta, TargetMeta> meta_;
  PropDynamicFn dynamic_;
  bool defaultSet_ = false;

  NumericMeta &numericMut();
  EnumMeta &enumMut();
};

/** "edgeCrease", "edge_crease" -> "Edge Crease"; "HTTPServer" -> "HTTP Server". */
std::string uiNameFromApiName(std::string_view apiName);

/**
 * Converts `value` to `def`'s kind: the one conversion point for every set.
 * - Int <- Int, or a Float that is integral and inside int64.
 * - Float <- Float or Int. NaN is an error for every numeric kind.
 * - Bool <- Bool, or an integral 0 or 1.
 * - Enum <- Enum or an integral known value, or String (a key; suggestions on a miss).
 * - Flags <- Flags or a non-negative integral (known bits only), or String keys joined
 *   by '|' ("" is no flags).
 * "Integral" includes a Float holding an integer: JS numbers arrive as Float.
 * - Vec <- the same Vec, or a scalar broadcast; Quat <- Quat or Vec4.
 * - Pointer <- Pointer (of the target struct type, when both are known), or None as
 *   the null pointer.
 * - String <- String, within maxLength.
 * Numeric components are then clamped to the hard range. A Collection takes no value.
 * A dynamic def is FailedPrecondition: resolve it for its owner first (resolved()).
 */
Result<PropValue> coerce(const PropertyDef &def, const PropValue &value);

} // namespace coredcc

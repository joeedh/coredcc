#pragma once

/** \file
 * Values as the text that users read and type, in a document's units, and the API struct
 * of UnitSettings.
 */

#include "coredcc/api/struct.h"
#include "coredcc/props/unit_settings.h"

#include <string>
#include <string_view>

namespace coredcc {

/**
 * A value of `def` as text: numbers in the settings' units with the def's precision, the
 * components of a vector with ", " between them, the UI name of an enum's item, and those
 * of flags with
 * ", " between them. A component of a vector is formatted with the vector's def.
 *
 * - A Percentage whose unit is Factor is in percent, whatever the settings say.
 * - A pointer is its debugging form (PropValue::toString).
 */
std::string
formatValue(const PropertyDef &def, const PropValue &value, const UnitSettings &settings);

/**
 * Reads what formatValue writes, and what coerce() takes: an enum's key, flags' keys.
 *
 * - A number without a unit is in the settings' unit. An Int in units is the nearest
 * whole number, if it is within 1e-9 of it, relatively; otherwise OutOfRange.
 * - A vector is its components with commas between them, or one number for all of them.
 * - A Bool is true, on, yes or 1, or false, off, no or 0.
 * - The result is coerced to the def, so it is within its range. Unsupported for Pointer
 * and Collection.
 */
Result<PropValue>
parseValue(const PropertyDef &def, std::string_view text, const UnitSettings &settings);

/** The value at a path, in the active scene's units (Context::units). */
Result<std::string> formatValue(Context &ctx, std::string_view path);
/** What setValue takes for the path: a Float for a component (its range is
 * for setValue), a Bool for an enum key. */
Result<PropValue> parseValue(Context &ctx, std::string_view path, std::string_view text);

/** The name of UnitSettings' API struct. */
inline constexpr const char *kUnitSettingsStruct = "coredcc.UnitSettings";
/** The member of a scene's struct that points to it. */
inline constexpr const char *kUnitsMember = "units";

/**
 * Registers the struct, unless it is there: `lengthUnit`, `angleUnit`, `timeUnit`,
 * `massUnit` and `factorUnit` (enums of the units' names), `fps` and `startFrame`.
 *
 * A scene of any type has units when its struct has a Pointer member `units` to it
 * (addUnitSettings), whose data is a UnitSettings.
 */
Status registerUnitSettings(Runtime &rt);

/** Adds the member `units`. */
void addUnitSettings(ApiStruct &st,
                     litestl::util::function<UnitSettings *(const PointerRef &self)> get);

} // namespace coredcc

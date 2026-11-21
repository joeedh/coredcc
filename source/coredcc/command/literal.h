#pragma once

/** \file
 * Property values as invocation literals: the text form command logs, LastUsed files and
 * `invoke` use. formatLiteral and parseLiteral round-trip for every kind but Pointer
 * and Collection.
 */

#include "coredcc/parse/invocation.h"
#include "coredcc/props/def.h"

#include <string>

namespace coredcc {

/**
 * `value` (already coerced to `def`) as a literal:
 * - Bool: `true` / `false`; Int: digits; Float: the shortest round-trip form
 *   (`nan`, `inf` and `-inf` as identifiers);
 * - String: a quoted string;
 * - Enum: the item's key; Flags: `"A|B"`, or `0` for none;
 * - vectors, Quat and Mat4: a tuple of numbers.
 * Pointer and Collection are Unsupported. An Enum value with no item is InvalidArgument.
 */
Result<std::string> formatLiteral(const PropertyDef &def, const PropValue &value);

/** A parsed literal as a value of `def`, coerced (so clamped). */
Result<PropValue> parseLiteral(const PropertyDef &def, const RawValue &raw);

} // namespace coredcc

#pragma once

/** \file
 * Property values as invocation literals: the text form command logs, LastUsed files and
 * `invoke` use. formatLiteral and parseLiteral round-trip for every kind but Collection.
 */

#include "coredcc/api/context.h"
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
 * - vectors, Quat and Mat4: a tuple of numbers;
 * - Pointer: its text (api/ref_text.h), quoted; `""` for null.
 * Collection is Unsupported. An Enum value with no item is InvalidArgument.
 */
Result<std::string>
formatLiteral(const Runtime &rt, const PropertyDef &def, const PropValue &value);

/** A parsed literal as a value of `def`, coerced (so clamped). A dynamic def must be
 * resolved for its owner first. */
Result<PropValue>
parseLiteral(const Runtime &rt, const PropertyDef &def, const RawValue &raw);

/** Whether a literal could be a value of `def`. A dynamic def's kind is all that counts:
 * its items and length depend on its owner. */
Status checkLiteral(const Runtime &rt, const PropertyDef &def, const RawValue &raw);

/** `ref` as a value of a Pointer def. A target whose struct inherits from the def's is
 * accepted. */
Result<PropValue> coercePointer(Runtime &rt, const PropertyDef &def, StableRef ref);

/**
 * As above for a Pointer, in a Context: the string is a reference's text, or a datapath,
 * which is resolved now (with the Context's prefix). A datapath names a struct, or a
 * Pointer member, whose target is meant. A target whose struct inherits from the def's is
 * accepted.
 */
Result<PropValue> parsePointer(Context &ctx, const PropertyDef &def, const RawValue &raw);

} // namespace coredcc

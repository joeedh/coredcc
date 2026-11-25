#pragma once

/** \file
 * What a datapath is, as JSON: for the property editors of script frontends.
 */

#include "coredcc/api/resolve.h"
#include "coredcc/base/json_writer.h"

#include <string>

namespace coredcc {

/** What `rp` is: a PropKind's name for a member, or "Struct", "Component", "EnumKey",
 * "Length". */
const char *pathKindName(const ResolvedPath &rp);

/**
 * What `path` is, as a JSON object. `canonical` is its address (api/ref_text.h), `kind`
 * is pathKindName's, and `struct` is the struct itself (Struct) or the one with the
 * member.
 *
 * - Struct: `uiName`, `description`, and `members`, the names (without Private ones).
 * - The rest: what writePropertyMeta writes, with `readOnly`.
 * - Component: also `component`, its index, and `of`, the member's kind.
 * - EnumKey: also `key` and `of`; the texts are the item's.
 * - Length: `target` is the items' struct.
 */
Result<std::string> pathMeta(Context &ctx, std::string_view path);

/**
 * A property's description, as fields of the object being written:
 * - `name`, `uiName`, `access` (names of flags) and `ui` (an object with what isn't
 * default);
 * - `description`, `icon` and `subtype`, if any;
 * - numbers: `precision`, and `hardMin`, `hardMax`, `softMin`, `softMax`, `step` and
 * `unit`, if any. A soft bound that isn't set is the hard one;
 * - Enum and Flags: `items`, each with `key`, `value`, `uiName`, and `description` and
 * `icon` if any;
 * - String: `maxLength`, if any;
 * - Pointer and Collection: `target`, the struct's name, if it's static;
 * - `default`: as scripts write it (an enum's key, a flags' keys, a pointer's text or
 * null). An infinite bound is left out, and integers past 2^53 are strings.
 */
void writePropertyMeta(JsonWriter &out, const Runtime &rt, const PropertyDef &def);

} // namespace coredcc

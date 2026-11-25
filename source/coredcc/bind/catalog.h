#pragma once

/** \file
 * Everything that is registered, as JSON: for generators of typed paths and the like
 * (ts/gen_paths.ts).
 */

#include "coredcc/api/context.h"
#include "coredcc/bind/facades.h"

#include <string>

namespace coredcc {

/**
 * The structs and commands of `ctx`'s Runtime, as a JSON object:
 * - `root`: the name of the struct of `ctx`'s root, if it is registered;
 * - `structs`: each with `name`, `uiName`, `description` if any, `parent` (a name) if any, and
 *   `members`, its own ones;
 * - `commands`: each with `id`, `uiName`, `description` and `icon` if any, `flags` (names), and
 *   `inputs` and `outputs`.
 *
 * Members, inputs and outputs have `kind` (a PropKind's name) and what writePropertyMeta writes;
 * members also `readOnly`. `dynamic` is true if that is only what is known without an instance: a
 * hook may change the range, the items and the access. A Pointer member without a static target has `targets`, the structs
 * that it may be. A Collection member's `active` says whether it has an active item.
 * Private members, inputs, outputs and commands are left out. The order is that of registration.
 */
std::string apiCatalog(Context &ctx);

namespace script {

/** coredcc::apiCatalog. */
StringResult apiCatalog(Context &ctx);

} // namespace script
} // namespace coredcc

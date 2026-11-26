#pragma once

/** \file
 * The commands that change the document's list of driver graphs (drive/driver_list.h).
 *
 * - `core.driver.add(graph)`: the document's list names a graph block, at its end.
 * The list is made first when there is none (undo removes it again). InvalidArgument for
 * a block that isn't a graph, AlreadyExists for one that a list names.
 * - `core.driver.remove(graph)`: no list names it any more. NotFound if none did.
 * Undo brings back every cell it removed, at its place.
 */

#include "coredcc/base/runtime.h"

namespace coredcc::drive {

inline constexpr const char *kAddDriverGraphCommand = "core.driver.add";
inline constexpr const char *kRemoveDriverGraphCommand = "core.driver.remove";

Status registerCommands(Runtime &rt);

} // namespace coredcc::drive

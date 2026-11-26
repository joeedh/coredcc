#pragma once

/** \file
 * The commands that change the graphs a scene lists.
 *
 * - `core.scene.addGraph(graph)`: the Context's scene lists a graph block, at its end.
 * InvalidArgument for a block that isn't a graph, AlreadyExists for one it lists.
 * - `core.scene.removeGraph(graph)`: it doesn't any more. NotFound if it didn't.
 */

#include "coredcc/base/runtime.h"

namespace coredcc::drive {

inline constexpr const char *kAddGraphCommand = "core.scene.addGraph";
inline constexpr const char *kRemoveGraphCommand = "core.scene.removeGraph";

Status registerCommands(Runtime &rt);

} // namespace coredcc::drive

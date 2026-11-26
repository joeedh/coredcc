#pragma once

/** \file
 * Undoable property sets (architecture §5, "Undoable set").
 */

#include "coredcc/api/context.h"
#include "coredcc/props/value.h"

#include <string_view>

namespace coredcc {

struct Path;

/**
 * Sets a datapath value through `ctx`'s executor, as a `core.property.set` command that
 * undo can reverse.
 *
 * - The command keeps the canonical path and the whole member's old and new values, so
 *   undo writes to the same member whatever `active` is by then.
 * - Calls with the same non-zero `gestureId`, from beginGesture, on the same member fold
 * into one undo step (a slider drag); 0 never folds.
 * - A member with the NoUndo flag is set directly, without a command.
 */
Status setValueUndo(Context &ctx,
                    std::string_view path,
                    const PropValue &value,
                    uint64_t gestureId = 0);
Status setValueUndo(Context &ctx,
                    const Path &path,
                    const PropValue &value,
                    uint64_t gestureId = 0);

/** A gesture's id, which no other gesture of `rt` has: one for each drag. */
uint64_t beginGesture(Runtime &rt);
/** Whether beginGesture gave it. */
bool isGesture(const Runtime &rt, uint64_t gestureId);
/**
 * Undoes what the gesture's sets did, and drops them, so that they can't be redone
 * (Escape). Only those at the history's head: not if another command was run, or one
 * undone, since. Members with the NoUndo flag stay as they are.
 */
Status cancelGesture(Context &ctx, uint64_t gestureId);

/** The id of the command setValueUndo runs. */
inline constexpr const char *kSetPropertyCommand = "core.property.set";

} // namespace coredcc

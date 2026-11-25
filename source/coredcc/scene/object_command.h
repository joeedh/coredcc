#pragma once

/* What the commands on objects share. Internal. */

#include "coredcc/command/block_edits.h"
#include "coredcc/scene/commands.h"

#include "litestl/binding/binding.h"

namespace coredcc::detail {

#define BIND_OBJECT_COMMAND(st)                                                          \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

/* The parts that all of them share. */
struct ChangesCommand {
  ObjectChanges changes;

  void undo(Context &ctx)
  {
    changes.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return changes.redo(ctx);
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + changes.memSize();
  }
  void discard() {}
};

Status registerDeltaCommands(Runtime &rt);
Status registerObjectCommands(Runtime &rt);

} // namespace coredcc::detail

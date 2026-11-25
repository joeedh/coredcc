#include "coredcc/command/library_commands.h"

#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/history_block.h"
#include "coredcc/lib/duplicate.h"

#include "litestl/binding/binding.h"

#include <memory>
#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

/* The undo side of a command that makes blocks. */
struct MadeBlocks {
  LibraryRef lib;
  CreatedBlocks created;

  void undo()
  {
    if (Library *l = lib.get()) {
      created.undo(*l);
    }
  }
  Status redo()
  {
    Library *l = lib.get();
    return l ? created.redo(*l) :
               Status::error(StatusCode::FailedPrecondition, "the Library is gone");
  }
  void discard()
  {
    if (Library *l = lib.get()) {
      created.discard(*l);
    }
  }
};

#define BIND_MAKER(st)                                                                   \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

struct DuplicateBlock {
  MadeBlocks made;

  Status canRun(Context &ctx) const
  {
    return detail::blockInput(ctx).status();
  }
  Status exec(Context &ctx)
  {
    Result<BlockHandle> h = detail::blockInput(ctx);
    if (!h.ok()) {
      return h.status();
    }
    Library &lib = *ctx.library();
    made.lib = lib.ref();
    Result<IdMap> ids = duplicateBlocks(lib, std::span<const BlockHandle>(&*h, 1));
    if (!ids.ok()) {
      return ids.status();
    }
    StableRef out;
    out.owner = OwnerKey::ofBlock(ids->find(lib.get(*h)->id()));
    made.created.add(lib, lib.find(out.owner.block));
    return ctx.outputs()->set("block", PropValue::ofPointer(out));
  }
  void undo(Context &)
  {
    made.undo();
  }
  Status redo(Context &)
  {
    return made.redo();
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard()
  {
    made.discard();
  }
  static lbt::Struct<DuplicateBlock> *defineBindings()
  {
    auto *st = new lbt::Struct<DuplicateBlock>("coredcc::DuplicateBlockCommand",
                                               sizeof(DuplicateBlock));
    BIND_MAKER(st);
    return st;
  }
};

/* What appendUndo asks for, and the answer. The spans are the caller's: exec runs at once. */
struct AppendRequest {
  std::span<const uint8_t> bytes;
  const LoadOptions *options = nullptr;
  AppendResult result;
};

struct AppendBlocks {
  std::shared_ptr<AppendRequest> request;
  MadeBlocks made;
  /* MissingBlock records: kept for a redo, but not undone. */
  Vector<HistoryBlock> missing;
  uint64_t size = 0;

  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    const std::shared_ptr<AppendRequest> req = std::exchange(request, nullptr);
    if (!req) {
      return Status::error(StatusCode::FailedPrecondition, "use appendUndo");
    }
    Result<AppendResult> r = appendLibrary(ctx, req->bytes, *req->options);
    if (!r.ok()) {
      return r.status();
    }
    Library &lib = *ctx.library();
    made.lib = lib.ref();
    size = req->bytes.size();
    const Vector<BlockHandle> &blocks = r->report.blocks;
    for (int i = 0; i < int(blocks.size()); i++) {
      if (i < int(r->report.records)) {
        made.created.add(lib, blocks[i]);
      }
      else {
        missing.append(HistoryBlock(lib, blocks[i]));
      }
    }
    req->result = std::move(r).value();
    return Status();
  }
  void undo(Context &)
  {
    made.undo();
  }
  Status redo(Context &)
  {
    return made.redo();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + size +
           uint64_t(made.created.size() + int(missing.size())) * sizeof(HistoryBlock);
  }
  void discard()
  {
    made.discard();
  }
  static lbt::Struct<AppendBlocks> *defineBindings()
  {
    auto *st = new lbt::Struct<AppendBlocks>("coredcc::AppendCommand", sizeof(AppendBlocks));
    BIND_MAKER(st);
    return st;
  }
};

} // namespace

Result<AppendResult> appendUndo(Context &ctx,
                                std::span<const uint8_t> bytes,
                                const LoadOptions &options)
{
  Runtime &rt = ctx.runtime();
  CommandExecutor *ex = ctx.executor();
  if (!ex) {
    return Status::error(StatusCode::FailedPrecondition, "appendUndo: the Context has no executor");
  }
  CDCC_TRY(detail::registerLibraryCommands(rt));
  const CommandDef *def = rt.ext<CommandRegistry>().find(kAppendCommand);
  AppendBlocks *impl = litestl::alloc::New<AppendBlocks>("coredcc::AppendCommand");
  auto request = std::make_shared<AppendRequest>();
  request->bytes = bytes;
  request->options = &options;
  impl->request = request;
  Result<Owned<Command>> cmd = own<Command>(rt, std::string(kAppendCommand) + "#impl", impl);
  if (!cmd.ok()) {
    return cmd.status();
  }
  Result<PropertyBag> out =
      ex->running() ? ex->runSub(ctx, std::move(cmd).value(), *def) :
                      ex->runInstance(ctx, *def, std::move(cmd).value(), def->newInputs());
  if (!out.ok()) {
    return out.status();
  }
  return std::move(request->result);
}

Status detail::registerLibraryCommands(Runtime &rt)
{
  CommandDef duplicate(kDuplicateBlockCommand);
  duplicate.uiName("Duplicate Block")
      .input(PropertyDef::pointerProp("block", StructId()))
      .output(PropertyDef::pointerProp("block", StructId()));
  CDCC_TRY(addCommandOnce<DuplicateBlock>(rt, std::move(duplicate)));
  CommandDef append(kAppendCommand);
  append.uiName("Append").flags(CommandFlags::Private);
  return addCommandOnce<AppendBlocks>(rt, std::move(append));
}

} // namespace coredcc

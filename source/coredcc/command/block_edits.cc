#include "coredcc/command/block_edits.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/history_block.h"

#include "litestl/binding/binding.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;

namespace detail {

Result<BlockHandle> blockInput(Context &ctx)
{
  Library *lib = ctx.library();
  if (!lib) {
    return Status::error(StatusCode::FailedPrecondition, "the Context has no Library");
  }
  const StableRef *ref = ctx.inputs()->get("block")->tryPointer();
  const bool isBlock = ref && ref->owner.kind == OwnerKey::Kind::Block;
  const BlockHandle h = isBlock ? lib->find(ref->owner.block) : BlockHandle();
  if (!h) {
    return Status::error(StatusCode::FailedPrecondition, "block: no such block");
  }
  return h;
}

} // namespace detail

namespace {

#define BIND_BLOCK_EDIT(st)                                                              \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

struct RenameBlock {
  HistoryBlock block;
  std::string before, after;

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
    block = HistoryBlock(*ctx.library(), *h);
    before = ctx.library()->get(*h)->name().c_str();
    after = ctx.inputs()->get("name")->asString().c_str();
    return apply(ctx, after);
  }
  Status apply(Context &ctx, const std::string &name)
  {
    CDCC_TRY(ctx.library()->rename(block.get(), name));
    ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(block.id()), "name");
    return Status();
  }
  void undo(Context &ctx)
  {
    if (Status s = apply(ctx, before); !s.ok()) {
      ctx.runtime().setStickyError(std::move(s));
    }
  }
  Status redo(Context &ctx)
  {
    return apply(ctx, after);
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + before.size() + after.size();
  }
  void discard()
  {
    block.release();
  }
  static lbt::Struct<RenameBlock> *defineBindings()
  {
    auto *st = new lbt::Struct<RenameBlock>("coredcc::RenameBlockCommand", sizeof(RenameBlock));
    BIND_BLOCK_EDIT(st);
    return st;
  }
};

struct UnlinkRef {
  HistoryBlock block;
  HistoryBlock target;
  BlockId before;
  int index = 0;
  TypeId attachment;

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
    const int64_t at = ctx.inputs()->get("index")->asInt();
    if (const std::string name = ctx.inputs()->get("attachment")->asString().c_str(); !name.empty())
    {
      attachment = TypeId::of(name);
    }
    const litestl::util::Vector<RefCell> *rows = lib.refTable(*h, attachment);
    if (!rows || at < 0 || at >= int64_t(rows->size())) {
      return Status::error(StatusCode::OutOfRange, "index: the block has no such reference");
    }
    index = int(at);
    before = (*rows)[index].target;
    CDCC_TRY(lib.forceUnlinkRef(*h, index, attachment));
    block = HistoryBlock(lib, *h);
    target = HistoryBlock(lib, lib.find(before));
    return Status();
  }
  Status apply(Context &ctx, BlockId to)
  {
    return ctx.library()->relinkRef(block.get(), index, to, attachment);
  }
  void undo(Context &ctx)
  {
    if (Status s = apply(ctx, before); !s.ok()) {
      ctx.runtime().setStickyError(std::move(s));
    }
  }
  Status redo(Context &ctx)
  {
    return apply(ctx, BlockId());
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard()
  {
    block.release();
    target.release();
  }
  static lbt::Struct<UnlinkRef> *defineBindings()
  {
    auto *st = new lbt::Struct<UnlinkRef>("coredcc::UnlinkRefCommand", sizeof(UnlinkRef));
    BIND_BLOCK_EDIT(st);
    return st;
  }
};

} // namespace

Status detail::registerBlockEdits(Runtime &rt)
{
  CommandDef rename(kRenameBlockCommand);
  rename.uiName("Rename Block")
      .input(PropertyDef::pointerProp("block", StructId()))
      .input(PropertyDef::stringProp("name"));
  CDCC_TRY(detail::addCommandOnce<RenameBlock>(rt, std::move(rename)));
  CommandDef unlink(kUnlinkRefCommand);
  unlink.uiName("Unlink Reference")
      .input(PropertyDef::pointerProp("block", StructId()))
      .input(PropertyDef::intProp("index"))
      .input(PropertyDef::stringProp("attachment"));
  return detail::addCommandOnce<UnlinkRef>(rt, std::move(unlink));
}

} // namespace coredcc

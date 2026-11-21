#include "coredcc/command/block_commands.h"

#include "coredcc/command/executor.h"

#include "litestl/binding/binding.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

Vector<BlockHandle> deletePreview(const Library &lib, BlockHandle target)
{
  Vector<BlockHandle> out;
  if (!lib.get(target)) {
    return out;
  }
  out.append(target);
  for (int i = 0; i < int(out.size()); i++) {
    const BlockId gone = lib.get(out[i])->id();
    for (BlockHandle user : lib.usersOf(out[i])) {
      if (out.contains(user)) {
        continue;
      }
      bool breaks = false;
      lib.foreachRef(user, [&](RefCell &cell) {
           breaks |= cell.kind == RefKind::Strong && cell.target == gone;
         })
          .ignore(); /* a provider failure is sticky; the preview stays partial */
      if (breaks) {
        out.append(user);
      }
    }
  }
  return out;
}

/* ---- CreatedBlocks ---- */

void CreatedBlocks::undo(Library &lib)
{
  for (int i = int(blocks_.size()) - 1; i >= 0; i--) {
    Status s = lib.markDeleted(blocks_[i]);
    if (!s.ok()) {
      lib.runtime().setStickyError(std::move(s));
    }
  }
  undone_ = true;
}

Status CreatedBlocks::redo(Library &lib)
{
  for (BlockHandle h : blocks_) {
    CDCC_TRY(lib.restoreFromHistory(h));
  }
  undone_ = false;
  return Status();
}

void CreatedBlocks::discard(Library &lib)
{
  if (undone_) {
    for (BlockHandle h : blocks_) {
      lib.destroyFromHistory(h).ignore(); /* already gone is fine */
    }
  }
  blocks_.clear();
}

namespace {

Library *libraryOf(Context &ctx, Status &err)
{
  if (!ctx.library()) {
    err = Status::error(StatusCode::FailedPrecondition, "the Context has no Library");
  }
  return ctx.library();
}

struct DeleteBlock {
  Library *lib = nullptr;
  Vector<BlockHandle> deleted;
  bool applied = false;

  Status canRun(Context &ctx) const
  {
    Status err;
    Library *l = libraryOf(ctx, err);
    if (!l) {
      return err;
    }
    const StableRef *ref = ctx.inputs()->get("block")->tryPointer();
    if (!ref || ref->owner.kind != OwnerKey::Kind::Block || !l->get(l->find(ref->owner.block))) {
      return Status::error(StatusCode::FailedPrecondition, "block: no such block");
    }
    return Status();
  }
  Status exec(Context &ctx)
  {
    lib = ctx.library();
    const BlockHandle target = lib->find(ctx.inputs()->get("block")->tryPointer()->owner.block);
    Vector<BlockHandle> preview = deletePreview(*lib, target);
    if (preview.size() > 1 && !ctx.inputs()->get("cascade")->asBool()) {
      std::string names;
      for (int i = 1; i < int(preview.size()); i++) {
        names += i > 1 ? ", " : "";
        names += lib->get(preview[i])->name().c_str();
      }
      return Status::error(StatusCode::FailedPrecondition,
                           ("deleting it would also delete " + names).c_str());
    }
    for (BlockHandle h : preview) {
      if (Status s = lib->markDeleted(h); !s.ok()) {
        undo(ctx); /* leave nothing half-done */
        return s;
      }
      deleted.append(h);
    }
    applied = true;
    ctx.outputs()->set("count", PropValue::ofInt(int64_t(deleted.size()))).ignore();
    return Status();
  }
  void undo(Context &ctx)
  {
    for (int i = int(deleted.size()) - 1; i >= 0; i--) {
      Status s = lib->restoreFromHistory(deleted[i]);
      if (!s.ok()) {
        ctx.runtime().setStickyError(std::move(s));
      }
    }
    applied = false;
  }
  Status redo(Context &)
  {
    for (BlockHandle h : deleted) {
      CDCC_TRY(lib->markDeleted(h));
    }
    applied = true;
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(deleted.size()) * sizeof(BlockHandle);
  }
  void discard()
  {
    if (applied) {
      for (BlockHandle h : deleted) {
        lib->destroyFromHistory(h).ignore();
      }
    }
    deleted.clear();
  }
  static lbt::Struct<DeleteBlock> *defineBindings()
  {
    auto *st = new lbt::Struct<DeleteBlock>("coredcc::DeleteBlockCommand", sizeof(DeleteBlock));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

struct Purge {
  Status canRun(Context &ctx) const
  {
    Status err;
    return libraryOf(ctx, err) ? Status() : err;
  }
  Status exec(Context &ctx)
  {
    Result<int> n = ctx.library()->collect();
    if (!n.ok()) {
      return n.status();
    }
    ctx.outputs()->set("count", PropValue::ofInt(*n)).ignore();
    return Status();
  }
  void undo(Context &) {}
  uint64_t memSize() const
  {
    return 0;
  }
  void discard() {}
  static lbt::Struct<Purge> *defineBindings()
  {
    auto *st = new lbt::Struct<Purge>("coredcc::PurgeCommand", sizeof(Purge));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

} // namespace

Status registerBlockCommands(Runtime &rt)
{
  CommandRegistry &reg = rt.ext<CommandRegistry>();
  if (!reg.find(kDeleteBlockCommand)) {
    CommandDef del(kDeleteBlockCommand);
    del.uiName("Delete Block")
        .input(PropertyDef::pointerProp("block", StructId()))
        .input(PropertyDef::boolProp("cascade").defaultValue(PropValue::ofBool(true)))
        .output(PropertyDef::intProp("count"));
    Result<const CommandDef *> d = addNativeCommand<DeleteBlock>(rt, std::move(del));
    if (!d.ok()) {
      return d.status();
    }
  }
  if (!reg.find(kPurgeCommand)) {
    CommandDef purge(kPurgeCommand);
    purge.uiName("Purge Unused").flags(CommandFlags::NoUndo).output(PropertyDef::intProp("count"));
    Result<const CommandDef *> d = addNativeCommand<Purge>(rt, std::move(purge));
    if (!d.ok()) {
      return d.status();
    }
  }
  return Status();
}

} // namespace coredcc

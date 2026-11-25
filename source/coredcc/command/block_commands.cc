#include "coredcc/command/block_commands.h"

#include "coredcc/command/attachment_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/library_commands.h"

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
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  for (int i = 0; i < int(out.size()); i++) {
    const BlockId gone = lib.get(out[i])->id();
    for (BlockHandle user : lib.usersOf(out[i])) {
      /* InHistory users are already gone, and Detached ones belong to the host
       * (destroyFromHistory guards their refs). */
      const BlockState state = lib.get(user)->state();
      if (out.contains(user) ||
          (state != BlockState::Live && state != BlockState::Placeholder))
      {
        continue;
      }
      bool breaks = false;
      lib.foreachRef(user, [&](RefCell &cell) {
           breaks |= cell.kind == RefKind::Strong && clients.canonical(cell.target) == gone;
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

namespace {

/* Destroys a block that leaves the history, unless it is gone or was recreated Live. A
 * failure has no caller to return to. */
void destroyDeleted(Library &lib, BlockHandle h)
{
  const Datablock *b = lib.get(h);
  if (!b || b->state() != BlockState::InHistory) {
    return;
  }
  if (Status s = lib.destroyFromHistory(h); !s.ok()) {
    lib.runtime().setStickyError(std::move(s));
  }
}

} // namespace

void CreatedBlocks::add(Library &lib, BlockHandle h)
{
  blocks_.append(HistoryBlock(lib, h));
}

void CreatedBlocks::undo(Library &lib)
{
  for (int i = int(blocks_.size()) - 1; i >= 0; i--) {
    Status s = lib.markDeleted(blocks_[i].get());
    if (!s.ok()) {
      lib.runtime().setStickyError(std::move(s));
    }
  }
  undone_ = true;
}

Status CreatedBlocks::redo(Library &lib)
{
  for (int i = 0; i < int(blocks_.size()); i++) {
    if (Status s = lib.restoreFromHistory(blocks_[i].get()); !s.ok()) {
      for (int j = i - 1; j >= 0; j--) {
        lib.markDeleted(blocks_[j].get()).ignore(); /* back to undone */
      }
      return s;
    }
  }
  undone_ = false;
  return Status();
}

void CreatedBlocks::discard(Library &lib)
{
  for (HistoryBlock &b : blocks_) {
    const BlockHandle h = b.get();
    b.release();
    if (undone_) {
      destroyDeleted(lib, h);
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
  Vector<HistoryBlock> deleted;
  bool applied = false;

  bool isDeleted(BlockHandle h) const
  {
    for (const HistoryBlock &b : deleted) {
      if (b.get() == h) {
        return true;
      }
    }
    return false;
  }
  Status canRun(Context &ctx) const
  {
    return detail::blockInput(ctx).status();
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
      deleted.append(HistoryBlock(*lib, h));
    }
    applied = true;
    ctx.outputs()->set("count", PropValue::ofInt(int64_t(deleted.size()))).ignore();
    return Status();
  }
  void undo(Context &ctx)
  {
    for (int i = int(deleted.size()) - 1; i >= 0; i--) {
      Status s = lib->restoreFromHistory(deleted[i].get());
      if (!s.ok()) {
        ctx.runtime().setStickyError(std::move(s));
      }
    }
    applied = false;
  }
  /* The graph may have changed since exec: a new Strong user must go too (cascade) or
   * the redo fails, rather than leaving its ref dangling. */
  Status redo(Context &ctx)
  {
    Vector<BlockHandle> preview = deletePreview(*lib, deleted[0].get());
    Vector<BlockHandle> extra;
    for (BlockHandle h : preview) {
      if (!isDeleted(h)) {
        extra.append(h);
      }
    }
    if (extra.size() > 0 && !ctx.inputs()->get("cascade")->asBool()) {
      return Status::error(StatusCode::FailedPrecondition,
                           ("redo: deleting it would now also delete '" +
                            std::string(lib->get(extra[0])->name().c_str()) + "'")
                               .c_str());
    }
    Vector<BlockHandle> all, did;
    for (const HistoryBlock &b : deleted) {
      all.append(b.get());
    }
    for (BlockHandle h : extra) {
      all.append(h);
    }
    Status s;
    for (int i = 0; i < int(all.size()) && s.ok(); i++) {
      s = lib->markDeleted(all[i]);
      if (s.ok()) {
        did.append(all[i]);
      }
    }
    if (!s.ok()) {
      for (int i = int(did.size()) - 1; i >= 0; i--) {
        lib->restoreFromHistory(did[i]).ignore();
      }
      return s;
    }
    for (BlockHandle h : extra) {
      deleted.append(HistoryBlock(*lib, h));
    }
    applied = true;
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(deleted.size()) * sizeof(HistoryBlock);
  }
  void discard()
  {
    for (HistoryBlock &b : deleted) {
      const BlockHandle h = b.get();
      b.release();
      if (applied) {
        destroyDeleted(*lib, h);
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
  CDCC_TRY(detail::registerBlockEdits(rt));
  CDCC_TRY(detail::registerAttachmentCommands(rt));
  return detail::registerLibraryCommands(rt);
}

} // namespace coredcc

#include "coredcc/command/block_commands.h"

#include "coredcc/command/attachment_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/library_commands.h"

#include "litestl/binding/binding.h"
#include "litestl/util/map.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {
using HandleSet = litestl::util::Map<BlockHandle, bool>;
}

Vector<BlockHandle> deletePreview(const Library &lib, BlockHandle target)
{
  return deletePreview(lib, std::span<const BlockHandle>(&target, 1));
}

Vector<BlockHandle> deletePreview(const Library &lib,
                                  std::span<const BlockHandle> targets)
{
  Vector<BlockHandle> out;
  HandleSet in;
  const auto add = [&](BlockHandle h) {
    if (lib.get(h) && !in.lookup_ptr(h)) {
      in.add(h, true);
      out.append(h);
    }
  };
  for (BlockHandle h : targets) {
    add(h);
  }
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  /* A user of many (a scene) has its cells looked at once: the Strong users, by target.
   */
  litestl::util::Map<BlockId, HandleSet> strong;
  HandleSet scanned;
  for (int i = 0; i < int(out.size()); i++) {
    const BlockId gone = lib.get(out[i])->id();
    for (BlockHandle user : lib.usersOf(out[i])) {
      /* InHistory users are already gone, and Detached ones belong to the host
       * (destroyFromHistory guards their refs). */
      const BlockState state = lib.get(user)->state();
      if (in.lookup_ptr(user) ||
          (state != BlockState::Live && state != BlockState::Placeholder))
      {
        continue;
      }
      if (scanned.add(user, true)) {
        lib.forEachRef(user,
                       [&](RefCell &cell) {
                         if (cell.kind == RefKind::Strong && !cell.target.isNull()) {
                           const BlockId target = clients.canonical(cell.target);
                           if (!strong.lookup_ptr(target)) {
                             strong.add(target, HandleSet());
                           }
                           strong.lookup_ptr(target)->add(user, true);
                         }
                       })
            .ignore(); /* a provider failure is sticky; the preview stays partial */
      }
      if (const HandleSet *users = strong.lookup_ptr(gone);
          users && users->lookup_ptr(user))
      {
        add(user);
      }
    }
  }
  return out;
}

/* ---- CreatedBlocks ---- */

namespace {

void destroyNow(Library &lib, const Vector<BlockHandle> &blocks)
{
  Status s = lib.destroyFromHistory(
      std::span<const BlockHandle>(blocks.data(), size_t(blocks.size())));
  if (!s.ok()) {
    lib.runtime().setStickyError(std::move(s));
  }
}

/* Destroys blocks that leave the history, but those that are gone or were recreated Live.
 * A failure has no caller to return to. */
void destroyDeleted(Library &lib, const Vector<BlockHandle> &blocks)
{
  Vector<BlockHandle> deleted;
  for (BlockHandle h : blocks) {
    const Datablock *b = lib.get(h);
    if (b && b->state() == BlockState::InHistory) {
      deleted.append(h);
    }
  }
  if (deleted.size() == 0) {
    return;
  }
  if (!lib.runtime().ext<detail::HistoryDestroys>().take(lib, deleted)) {
    destroyNow(lib, deleted);
  }
}

/* Deletes them all, or none. */
Status deleteAll(Library &lib, const Vector<BlockHandle> &blocks)
{
  for (int i = 0; i < int(blocks.size()); i++) {
    if (Status s = lib.markDeleted(blocks[i]); !s.ok()) {
      for (int j = i - 1; j >= 0; j--) {
        lib.restoreFromHistory(blocks[j]).ignore(); /* it was deleted a moment ago */
      }
      return s;
    }
  }
  return Status();
}

} // namespace

namespace detail {

bool HistoryDestroys::take(Library &lib, const Vector<BlockHandle> &blocks)
{
  if (depth_ == 0) {
    return false;
  }
  for (Batch &b : batches_) {
    if (b.lib.get() == &lib) {
      for (BlockHandle h : blocks) {
        b.blocks.append(h);
      }
      return true;
    }
  }
  batches_.append(Batch{lib.ref(), blocks});
  return true;
}

void HistoryDestroys::close()
{
  if (--depth_ > 0) {
    return;
  }
  const Vector<Batch> batches = std::move(batches_);
  batches_ = Vector<Batch>();
  for (const Batch &b : batches) {
    if (Library *lib = b.lib.get()) {
      destroyDeleted(*lib, b.blocks);
    }
  }
}

} // namespace detail

void CreatedBlocks::add(Library &lib, BlockHandle h)
{
  Made m;
  m.block = HistoryBlock(lib, h);
  m.adopted = lib.adopted(h);
  blocks_.append(std::move(m));
}

Status CreatedBlocks::undoOne(Library &lib, Made &m)
{
  if (!m.adopted) {
    return lib.markDeleted(m.block.get());
  }
  Result<BlockHandle> kept = lib.unadopt(m.block.get());
  if (!kept.ok()) {
    return kept.status();
  }
  m.kept = LibraryPin(lib, *kept);
  return Status();
}

Status CreatedBlocks::redoOne(Library &lib, Made &m)
{
  if (!m.adopted) {
    return lib.restoreFromHistory(m.block.get());
  }
  CDCC_TRY(lib.readopt(m.block.get(), m.kept.handle()));
  m.kept.reset();
  return Status();
}

void CreatedBlocks::undo(Library &lib)
{
  for (int i = int(blocks_.size()) - 1; i >= 0; i--) {
    if (Status s = undoOne(lib, blocks_[i]); !s.ok()) {
      lib.runtime().setStickyError(std::move(s));
    }
  }
  undone_ = true;
}

Status CreatedBlocks::redo(Library &lib)
{
  for (int i = 0; i < int(blocks_.size()); i++) {
    if (Status s = redoOne(lib, blocks_[i]); !s.ok()) {
      for (int j = i - 1; j >= 0; j--) {
        undoOne(lib, blocks_[j]).ignore(); /* back to undone */
      }
      return s;
    }
  }
  undone_ = false;
  return Status();
}

void CreatedBlocks::discard(Library &lib)
{
  Vector<BlockHandle> gone;
  for (Made &m : blocks_) {
    const BlockHandle h = m.block.get();
    m.block.release();
    if (undone_ && !m.adopted) {
      gone.append(h);
    }
  }
  destroyDeleted(lib, gone);
  blocks_.clear();
}

uint64_t CreatedBlocks::memSize() const
{
  uint64_t n = uint64_t(blocks_.size()) * sizeof(Made);
  for (int i = 0; undone_ && i < int(blocks_.size()); i++) {
    const Made &m = blocks_[i];
    if (const Library *lib = m.block.library()) {
      n += lib->memSize(m.adopted ? m.kept.handle() : m.block.get());
    }
  }
  return n;
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
  LibraryRef lib;
  /* The targets, then what went with them. */
  Vector<HistoryBlock> deleted;
  int targets = 0;
  bool applied = false;

  Vector<BlockHandle> handles(int n) const
  {
    Vector<BlockHandle> out;
    for (int i = 0; i < n; i++) {
      out.append(deleted[i].get());
    }
    return out;
  }
  void take(Library &l, const Vector<BlockHandle> &blocks)
  {
    for (BlockHandle h : blocks) {
      deleted.append(HistoryBlock(l, h));
    }
  }
  Status canRun(Context &ctx) const
  {
    return detail::blocksInput(ctx).status();
  }
  Status exec(Context &ctx)
  {
    Result<Vector<BlockHandle>> asked = detail::blocksInput(ctx);
    if (!asked.ok()) {
      return asked.status();
    }
    Library &l = *ctx.library();
    lib = l.ref();
    targets = int(asked->size());
    const Vector<BlockHandle> all =
        deletePreview(l, std::span<const BlockHandle>(asked->data(), size_t(targets)));
    if (int(all.size()) > targets && !ctx.inputs()->get("cascade")->asBool()) {
      return Status::error(
          StatusCode::FailedPrecondition,
          ("deleting it would also delete " + names(l, all, targets)).c_str());
    }
    CDCC_TRY(deleteAll(l, all));
    take(l, all);
    applied = true;
    ctx.outputs()->set("count", PropValue::ofInt(int64_t(all.size()))).ignore();
    return Status();
  }
  static std::string names(const Library &l, const Vector<BlockHandle> &blocks, int from)
  {
    constexpr int kMost = 20;
    std::string s;
    for (int i = from; i < int(blocks.size()) && i < from + kMost; i++) {
      s += i > from ? ", " : "";
      s += l.get(blocks[i])->name().c_str();
    }
    const int more = int(blocks.size()) - from - kMost;
    return more > 0 ? s + " and " + std::to_string(more) + " more" : s;
  }
  void undo(Context &ctx)
  {
    Result<Library *> l = detail::libraryOf(ctx, lib);
    if (!l.ok()) {
      ctx.runtime().setStickyError(l.status());
      return;
    }
    for (int i = int(deleted.size()) - 1; i >= 0; i--) {
      if (Status s = (*l)->restoreFromHistory(deleted[i].get()); !s.ok()) {
        ctx.runtime().setStickyError(std::move(s));
      }
    }
    applied = false;
  }
  /* The graph may have changed since exec: a new Strong user must go too (cascade) or
   * the redo fails, rather than leaving its ref dangling. */
  Status redo(Context &ctx)
  {
    Result<Library *> l = detail::libraryOf(ctx, lib);
    if (!l.ok()) {
      return l.status();
    }
    const Vector<BlockHandle> first = handles(targets);
    Vector<BlockHandle> all = handles(int(deleted.size()));
    HandleSet known;
    for (BlockHandle h : all) {
      known.add(h, true);
    }
    Vector<BlockHandle> extra;
    for (BlockHandle h : deletePreview(
             **l, std::span<const BlockHandle>(first.data(), size_t(first.size()))))
    {
      if (!known.lookup_ptr(h)) {
        extra.append(h);
        all.append(h);
      }
    }
    if (extra.size() > 0 && !ctx.inputs()->get("cascade")->asBool()) {
      return Status::error(
          StatusCode::FailedPrecondition,
          ("redo: deleting it would now also delete " + names(**l, extra, 0)).c_str());
    }
    CDCC_TRY(deleteAll(**l, all));
    take(**l, extra);
    applied = true;
    return Status();
  }
  uint64_t memSize() const
  {
    uint64_t n = sizeof(*this) + uint64_t(deleted.size()) * sizeof(HistoryBlock);
    const Library *l = lib.get();
    for (int i = 0; l && applied && i < int(deleted.size()); i++) {
      n += l->memSize(deleted[i].get());
    }
    return n;
  }
  void discard()
  {
    Library *l = lib.get();
    const Vector<BlockHandle> gone =
        l && applied ? handles(int(deleted.size())) : Vector<BlockHandle>();
    deleted.clear(); /* unpins them */
    if (l) {
      destroyDeleted(*l, gone);
    }
  }
  static lbt::Struct<DeleteBlock> *defineBindings()
  {
    auto *st =
        new lbt::Struct<DeleteBlock>("coredcc::DeleteBlockCommand", sizeof(DeleteBlock));
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
  void undo(Context &)
  {
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard()
  {
  }
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
        .input(PropertyDef::stringProp("blocks"))
        .input(PropertyDef::boolProp("cascade").defaultValue(PropValue::ofBool(true)))
        .output(PropertyDef::intProp("count"));
    Result<const CommandDef *> d = addNativeCommand<DeleteBlock>(rt, std::move(del));
    if (!d.ok()) {
      return d.status();
    }
  }
  if (!reg.find(kPurgeCommand)) {
    CommandDef purge(kPurgeCommand);
    purge.uiName("Purge Unused")
        .flags(CommandFlags::NoUndo)
        .output(PropertyDef::intProp("count"));
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

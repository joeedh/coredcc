#include "coredcc/command/set_property.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/resolve.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/history_block.h"
#include "coredcc/lib/library.h"
#include "coredcc/parse/path.h"
#include "coredcc/parse/path_cache.h"

#include "litestl/binding/binding.h"

#include <utility>

namespace coredcc {

namespace lbt = litestl::binding::types;

namespace {

/* The command behind setValueUndo. exec writes through the path as given (so component
 * and enum-key writes work); undo and redo write the whole member by canonical ref.
 * Pointer values pin their target blocks (history pins, §8a) until the discard. */
struct SetProperty {
  Path path;
  PropValue input;
  uint64_t gesture = 0;
  StableRef ref;
  PropValue oldValue;
  PropValue newValue;
  /* With a ValueAccessor::save, instead of the values. */
  ValueAccessor::Stored oldStored;
  ValueAccessor::Stored newStored;
  /* With a PointerAccessor::cell: the linked ids, also of blocks that aren't Live (those
   * read as null). */
  bool hasCell = false;
  BlockId oldTarget;
  BlockId newTarget;
  /* Null when the Context had no Library. */
  LibraryRef lib;
  bool inLibrary = false;
  litestl::util::Vector<HistoryBlock> pins;

  void pinTarget(Context &ctx, const PropValue &v)
  {
    const StableRef *r = v.tryPointer();
    if (r && r->owner.kind == OwnerKey::Kind::Block) {
      pinBlock(ctx, r->owner.block);
    }
  }
  void pinBlock(Context &ctx, BlockId id)
  {
    Library *l = ctx.library();
    const BlockHandle h = l ? l->find(id) : BlockHandle();
    bool has = !h;
    for (const HistoryBlock &p : pins) {
      has |= p.get() == h;
    }
    if (!has) {
      pins.append(HistoryBlock(*l, h));
    }
  }
  /* Whether `ctx` is of the Library the value was set in. */
  Status sameLibrary(Context &ctx) const
  {
    return inLibrary ? detail::libraryOf(ctx, lib).status() : Status();
  }

  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    Result<ResolvedPath> rp = resolveForWrite(ctx, path);
    if (!rp.ok()) {
      return rp.status();
    }
    ref = rp->canonical;
    inLibrary = ctx.library() != nullptr;
    lib = inLibrary ? ctx.library()->ref() : LibraryRef();
    Result<PropValue> old = readWhole(ctx);
    if (!old.ok()) {
      return old.status();
    }
    oldValue = *old;
    if (const RefCell *cell = cellOf(ctx, *rp)) {
      hasCell = true;
      oldTarget = cell->target;
    }
    oldStored = stored(ctx, *rp);
    CDCC_TRY(setValue(ctx, *rp, input));
    newStored = stored(ctx, *rp);
    Result<PropValue> now = readWhole(ctx);
    if (!now.ok()) {
      return now.status();
    }
    newValue = *now;
    pinTarget(ctx, oldValue);
    pinTarget(ctx, newValue);
    if (hasCell) {
      const RefCell *cell = cellOf(ctx, *rp);
      newTarget = cell ? cell->target : BlockId();
      pinBlock(ctx, oldTarget);
    }
    return Status();
  }
  void undo(Context &ctx)
  {
    Status s = writeStatus(ctx, oldValue, oldTarget, oldStored.restore);
    if (!s.ok()) {
      ctx.runtime().setStickyError(std::move(s)); /* undo can't return a Status */
    }
  }
  Status redo(Context &ctx)
  {
    return writeStatus(ctx, newValue, newTarget, newStored.restore);
  }
  uint64_t memSize() const
  {
    uint64_t n = sizeof(*this) + uint64_t(ref.key.size()) * 32 +
                 uint64_t(pins.size()) * sizeof(HistoryBlock) + oldStored.bytes +
                 newStored.bytes;
    for (const PropValue *v : {&oldValue, &newValue}) {
      if (v->type() == PropValue::Type::String) {
        n += uint64_t(v->asString().size());
      }
    }
    return n;
  }
  void discard()
  {
    pins.clear();
  }
  uint64_t foldKey() const
  {
    if (gesture == 0) {
      return 0;
    }
    const uint64_t k = ref.computeHash() ^ (gesture * 0x9E3779B97F4A7C15ull);
    return k == 0 ? 1 : k;
  }
  bool foldFrom(Command next)
  {
    SetProperty *n = next.as<SetProperty>();
    if (!n || n->gesture != gesture || !(n->ref == ref)) {
      return false;
    }
    newValue = n->newValue;
    newTarget = n->newTarget;
    newStored = std::move(n->newStored);
    for (HistoryBlock &p : n->pins) {
      pins.append(std::move(p));
    }
    n->pins.clear();
    return true;
  }

  Result<PropValue> readWhole(Context &ctx) const
  {
    Result<ResolvedPath> rp = resolve(ctx, ref);
    if (!rp.ok()) {
      return rp.status();
    }
    return getValue(ctx, *rp);
  }
  static RefCell *cellOf(Context &ctx, const ResolvedPath &rp)
  {
    const bool has = rp.kind == ResolvedPath::Kind::Member &&
                     rp.member->kind() == MemberKind::Pointer &&
                     rp.member->pointer().cell && ctx.library();
    return has ? rp.member->pointer().cell(rp.ptr, ctx) : nullptr;
  }
  static ValueAccessor::Stored stored(Context &ctx, const ResolvedPath &rp)
  {
    const bool has =
        rp.member && rp.member->kind() == MemberKind::Value && rp.member->value().save;
    return has ? rp.member->value().save(rp.ptr, ctx) : ValueAccessor::Stored();
  }
  Status writeStatus(Context &ctx,
                     const PropValue &v,
                     const BlockId &target,
                     const ValueAccessor::RestoreFn &restore) const
  {
    CDCC_TRY(sameLibrary(ctx));
    Result<ResolvedPath> rp = resolve(ctx, ref);
    if (!rp.ok()) {
      return rp.status();
    }
    if (restore) {
      CDCC_TRY(restore(rp->ptr, ctx));
      written(ctx, *rp);
      return Status();
    }
    RefCell *cell = hasCell && !target.isNull() ? cellOf(ctx, *rp) : nullptr;
    if (!cell || isLiveTarget(*ctx.library(), v, target)) {
      return setValue(ctx, *rp, v);
    }
    /* A link to a block that isn't Live, or wasn't when it was read: the cell itself. */
    CDCC_TRY(ctx.library()->writeRef(*cell, target));
    written(ctx, *rp);
    return Status();
  }
  /* What setValue does after a write. */
  static void written(Context &ctx, const ResolvedPath &rp)
  {
    ctx.runtime().ext<ChangeBus>().notify(rp.canonical.owner, rp.member->name());
    if (rp.member->updateFn()) {
      rp.member->updateFn()(rp.ptr, ctx);
    }
  }
  static bool isLiveTarget(const Library &l, const PropValue &v, const BlockId &target)
  {
    const StableRef *r = v.tryPointer();
    const Datablock *b = l.get(l.find(target));
    return r && r->owner.kind == OwnerKey::Kind::Block && b &&
           b->state() == BlockState::Live;
  }

  static lbt::Struct<SetProperty> *defineBindings()
  {
    auto *st =
        new lbt::Struct<SetProperty>("coredcc::SetPropertyCommand", sizeof(SetProperty));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    BIND_STRUCT_METHOD(st, foldKey, MARGS());
    BIND_STRUCT_METHOD(st, foldFrom, MARGS("next"));
    return st;
  }
};

constexpr const char *kImpl = "core.property.set#impl";

/* The last gesture's id. */
struct Gestures {
  static constexpr const char *kExtName = "coredcc.Gestures";
  explicit Gestures(Runtime *)
  {
  }
  uint64_t last = 0;
};

Result<const CommandDef *> setPropertyDef(Runtime &rt)
{
  if (const CommandDef *d = rt.ext<CommandRegistry>().find(kSetPropertyCommand)) {
    if (d->implType() != TypeId::of(kImpl)) {
      return Status::error(StatusCode::AlreadyExists,
                           "core.property.set is registered with another implementation");
    }
    return d;
  }
  CommandDef def(kSetPropertyCommand);
  def.uiName("Set Property").flags(CommandFlags::Private);
  return addNativeCommand<SetProperty>(rt, std::move(def));
}

} // namespace

Status
setValueUndo(Context &ctx, const Path &path, const PropValue &value, uint64_t gestureId)
{
  Result<ResolvedPath> rp = resolveForWrite(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  if (rp->member && rp->member->def().hasAccess(PropAccess::NoUndo)) {
    return setValue(ctx, *rp, value);
  }
  CommandExecutor *ex = ctx.executor();
  if (!ex) {
    return Status::error(StatusCode::FailedPrecondition,
                         "setValueUndo: the Context has no executor");
  }
  Result<const CommandDef *> def = setPropertyDef(ctx.runtime());
  if (!def.ok()) {
    return def.status();
  }
  SetProperty *impl = litestl::alloc::New<SetProperty>("coredcc::SetPropertyCommand");
  impl->path = path;
  impl->input = value;
  impl->gesture = gestureId;
  Result<Owned<Command>> cmd = own<Command>(ctx.runtime(), kImpl, impl);
  if (!cmd.ok()) {
    return cmd.status();
  }
  /* Inside a command's exec it is one of that command's sub-commands. */
  Result<PropertyBag> out =
      ex->running()
          ? ex->runSub(ctx, std::move(cmd).value(), **def)
          : ex->runInstance(ctx, **def, std::move(cmd).value(), (*def)->newInputs());
  return out.ok() ? Status() : out.status();
}

uint64_t beginGesture(Runtime &rt)
{
  return ++rt.ext<Gestures>().last;
}

bool isGesture(const Runtime &rt, uint64_t gestureId)
{
  const Gestures *g = rt.findExt<Gestures>();
  return g && gestureId > 0 && gestureId <= g->last;
}

Status cancelGesture(Context &ctx, uint64_t gestureId)
{
  if (!isGesture(ctx.runtime(), gestureId)) {
    return Status::error(StatusCode::InvalidArgument,
                         "cancelGesture: not beginGesture's id");
  }
  if (!ctx.executor()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "cancelGesture: the Context has no executor");
  }
  return ctx.executor()->revertHead(ctx, [gestureId](CommandRecord &rec) {
    const SetProperty *set = rec.command().as<SetProperty>();
    return set && set->gesture == gestureId;
  });
}

Status setValueUndo(Context &ctx,
                    std::string_view path,
                    const PropValue &value,
                    uint64_t gestureId)
{
  Result<PathPtr> p = ctx.runtime().ext<PathCache>().get(path);
  if (!p.ok()) {
    return p.status();
  }
  return setValueUndo(ctx, **p, value, gestureId);
}

} // namespace coredcc

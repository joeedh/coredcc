#include "coredcc/command/set_property.h"

#include "coredcc/api/resolve.h"
#include "coredcc/command/executor.h"
#include "coredcc/lib/library.h"
#include "coredcc/parse/path.h"
#include "coredcc/parse/path_cache.h"

#include "litestl/binding/binding.h"

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
  Library *lib = nullptr;
  litestl::util::Vector<BlockHandle> pins;

  void pinTarget(Context &ctx, const PropValue &v)
  {
    const StableRef *r = v.tryPointer();
    Library *l = ctx.library();
    if (!r || !l || r->owner.kind != OwnerKey::Kind::Block) {
      return;
    }
    const BlockHandle h = l->find(r->owner.block);
    if (l->get(h)) {
      l->pin(h);
      lib = l;
      pins.append(h);
    }
  }

  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    Result<ResolvedPath> rp = resolve(ctx, path);
    if (!rp.ok()) {
      return rp.status();
    }
    ref = rp->canonical;
    Result<PropValue> old = readWhole(ctx);
    if (!old.ok()) {
      return old.status();
    }
    oldValue = *old;
    CDCC_TRY(setValue(ctx, *rp, input));
    Result<PropValue> now = readWhole(ctx);
    if (!now.ok()) {
      return now.status();
    }
    newValue = *now;
    pinTarget(ctx, oldValue);
    pinTarget(ctx, newValue);
    return Status();
  }
  void undo(Context &ctx)
  {
    write(ctx, oldValue);
  }
  Status redo(Context &ctx)
  {
    return writeStatus(ctx, newValue);
  }
  uint64_t memSize() const
  {
    uint64_t n = sizeof(*this) + uint64_t(ref.key.size()) * 32 + uint64_t(pins.size()) * 8;
    for (const PropValue *v : {&oldValue, &newValue}) {
      if (v->type() == PropValue::Type::String) {
        n += uint64_t(v->asString().size());
      }
    }
    return n;
  }
  void discard()
  {
    for (BlockHandle h : pins) {
      lib->unpin(h);
    }
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
    for (BlockHandle h : n->pins) {
      pins.append(h); /* n's discard then unpins nothing */
    }
    n->pins.clear();
    if (!lib) {
      lib = n->lib;
    }
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
  Status writeStatus(Context &ctx, const PropValue &v) const
  {
    Result<ResolvedPath> rp = resolve(ctx, ref);
    if (!rp.ok()) {
      return rp.status();
    }
    return setValue(ctx, *rp, v);
  }
  void write(Context &ctx, const PropValue &v) const
  {
    Status s = writeStatus(ctx, v);
    if (!s.ok()) {
      ctx.runtime().setStickyError(std::move(s)); /* undo can't return a Status */
    }
  }

  static lbt::Struct<SetProperty> *defineBindings()
  {
    auto *st = new lbt::Struct<SetProperty>("coredcc::SetPropertyCommand", sizeof(SetProperty));
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

constexpr const char *kImpl = "core.setProperty#impl";

Result<const CommandDef *> setPropertyDef(Runtime &rt)
{
  if (const CommandDef *d = rt.ext<CommandRegistry>().find(kSetPropertyCommand)) {
    if (d->implType() != TypeId::of(kImpl)) {
      return Status::error(StatusCode::AlreadyExists,
                           "core.setProperty is registered with another implementation");
    }
    return d;
  }
  CommandDef def(kSetPropertyCommand);
  def.uiName("Set Property").flags(CommandFlags::Private);
  return addNativeCommand<SetProperty>(rt, std::move(def));
}

} // namespace

Status setValueUndo(Context &ctx, const Path &path, const PropValue &value, uint64_t gestureId)
{
  Result<ResolvedPath> rp = resolve(ctx, path);
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
  Result<PropertyBag> out = ex->running()
                                ? ex->runSub(ctx, std::move(cmd).value(), **def)
                                : ex->runInstance(ctx, **def, std::move(cmd).value(),
                                                  (*def)->newInputs());
  return out.ok() ? Status() : out.status();
}

Status setValueUndo(Context &ctx, std::string_view path, const PropValue &value, uint64_t gestureId)
{
  Result<PathPtr> p = ctx.runtime().ext<PathCache>().get(path);
  if (!p.ok()) {
    return p.status();
  }
  return setValueUndo(ctx, **p, value, gestureId);
}

} // namespace coredcc

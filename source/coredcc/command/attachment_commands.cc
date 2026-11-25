#include "coredcc/command/attachment_commands.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/history_block.h"
#include "coredcc/lib/attachment.h"

#include "litestl/binding/binding.h"

#include <memory>
#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

const AttachmentTypeRegistry::Entry *findType(Runtime &rt, TypeId type)
{
  return rt.ext<AttachmentTypeRegistry>().find(type);
}

Status noType(std::string_view name)
{
  return Status::error(
      StatusCode::NotFound,
      ("type: no attachment type '" + std::string(name) + "' is registered").c_str());
}

/* Data that belongs to a command or to the caller of attachUndo while it isn't attached. */
struct DetachedData {
  Runtime *rt = nullptr;
  TypeId type;
  void *data = nullptr;

  void destroy()
  {
    if (data) {
      AttachmentProvider p = findType(*rt, type)->provider.get();
      p.destroy(std::exchange(data, nullptr));
    }
  }
};

/* One attach or detach, done and undone. */
struct AttachmentEdit {
  HistoryBlock block;
  std::shared_ptr<DetachedData> held = std::make_shared<DetachedData>();
  /* What the detached data references: a purge must not free it. */
  Vector<HistoryBlock> targets;

  void begin(Context &ctx, BlockHandle h, TypeId type)
  {
    held->rt = &ctx.runtime();
    held->type = type;
    block = HistoryBlock(*ctx.library(), h);
  }
  Status attach(Context &ctx)
  {
    CDCC_TRY(ctx.library()->attach(block.get(), held->type, held->data));
    held->data = nullptr;
    targets.clear();
    notify(ctx);
    return Status();
  }
  Status detach(Context &ctx)
  {
    Library &lib = *ctx.library();
    Result<void *> d = lib.detach(block.get(), held->type);
    if (!d.ok()) {
      return d.status();
    }
    held->data = *d;
    const AttachmentProvider p = findType(*held->rt, held->type)->provider.get();
    const bool stickyBefore = held->rt->hasStickyError();
    if (p.hasForeachRef()) {
      struct Pinner {
        Library &lib;
        Vector<HistoryBlock> &to;
      } pinner{lib, targets};
      RefVisitor v(
          [](void *ctx, RefCell &cell) {
            Pinner &p = *static_cast<Pinner *>(ctx);
            if (const BlockHandle t = p.lib.find(cell.target)) {
              p.to.append(HistoryBlock(p.lib, t));
            }
          },
          &pinner);
      p.foreachRef(held->data, v);
    }
    if (!stickyBefore && held->rt->hasStickyError()) {
      /* It threw: what the data references isn't known, so put it back. */
      targets.clear();
      lib.attach(block.get(), held->type, std::exchange(held->data, nullptr)).ignore();
      return held->rt->takeStickyError();
    }
    notify(ctx);
    return Status();
  }
  void notify(Context &ctx)
  {
    ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(block.id()), "attachments");
  }
  void discard()
  {
    held->destroy();
    targets.clear();
    block.release();
  }
};

#define BIND_ATTACHMENT_COMMAND(st)                                                      \
  BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));                                          \
  BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));                                            \
  BIND_STRUCT_METHOD(st, memSize, MARGS());                                              \
  BIND_STRUCT_METHOD(st, discard, MARGS())

template <bool Attach> struct EditCommand {
  AttachmentEdit edit;

  Status canRun(Context &ctx) const
  {
    CDCC_TRY(detail::blockInput(ctx).status());
    const std::string name = ctx.inputs()->get("type")->asString().c_str();
    const AttachmentTypeRegistry::Entry *e = findType(ctx.runtime(), TypeId::of(name));
    if (!e) {
      return noType(name);
    }
    if (Attach && !e->provider->hasCreate()) {
      return Status::error(StatusCode::Unsupported,
                           ("attachment type '" + name + "' has no create").c_str());
    }
    return Status();
  }
  Status exec(Context &ctx)
  {
    const TypeId type = TypeId::of(ctx.inputs()->get("type")->asString().c_str());
    Result<BlockHandle> h = detail::blockInput(ctx);
    if (!h.ok()) {
      return h.status();
    }
    edit.begin(ctx, *h, type);
    if constexpr (Attach) {
      AttachmentProvider p = findType(ctx.runtime(), type)->provider.get();
      edit.held->data = p.create();
      if (ctx.runtime().hasStickyError()) {
        return ctx.runtime().takeStickyError(); /* the discard destroys the data */
      }
      if (!edit.held->data) {
        return Status::error(StatusCode::Internal, "the attachment type's create gave no data");
      }
      return edit.attach(ctx);
    }
    else {
      return edit.detach(ctx);
    }
  }
  Status apply(Context &ctx, bool attach)
  {
    return attach ? edit.attach(ctx) : edit.detach(ctx);
  }
  void undo(Context &ctx)
  {
    if (Status s = apply(ctx, !Attach); !s.ok()) {
      ctx.runtime().setStickyError(std::move(s));
    }
  }
  Status redo(Context &ctx)
  {
    return apply(ctx, Attach);
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(edit.targets.size()) * sizeof(HistoryBlock);
  }
  void discard()
  {
    edit.discard();
  }
  static lbt::Struct<EditCommand> *defineBindings()
  {
    auto *st = new lbt::Struct<EditCommand>(
        Attach ? "coredcc::AttachCommand" : "coredcc::DetachCommand", sizeof(EditCommand));
    BIND_ATTACHMENT_COMMAND(st);
    return st;
  }
};

/* attachUndo's: the data is given. */
struct AttachData {
  AttachmentEdit edit;
  BlockHandle target;

  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    if (!ctx.library()) {
      return Status::error(StatusCode::FailedPrecondition, "the Context has no Library");
    }
    edit.begin(ctx, target, edit.held->type);
    return edit.attach(ctx);
  }
  void undo(Context &ctx)
  {
    if (Status s = edit.detach(ctx); !s.ok()) {
      ctx.runtime().setStickyError(std::move(s));
    }
  }
  Status redo(Context &ctx)
  {
    return edit.attach(ctx);
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(edit.targets.size()) * sizeof(HistoryBlock);
  }
  void discard()
  {
    edit.discard();
  }
  static lbt::Struct<AttachData> *defineBindings()
  {
    auto *st = new lbt::Struct<AttachData>("coredcc::AttachDataCommand", sizeof(AttachData));
    BIND_ATTACHMENT_COMMAND(st);
    return st;
  }
};

} // namespace

Status attachUndo(Context &ctx, BlockHandle h, TypeId type, void *data)
{
  Runtime &rt = ctx.runtime();
  if (!findType(rt, type)) {
    const char *name = rt.nameOf(type);
    return noType(name ? name : "?");
  }
  auto held = std::make_shared<DetachedData>();
  held->rt = &rt;
  held->type = type;
  held->data = data;
  const auto run = [&]() -> Status {
    CommandExecutor *ex = ctx.executor();
    if (!ex) {
      return Status::error(StatusCode::FailedPrecondition,
                           "attachUndo: the Context has no executor");
    }
    CDCC_TRY(detail::registerAttachmentCommands(rt));
    const CommandDef *def = rt.ext<CommandRegistry>().find(kAttachDataCommand);
    AttachData *impl = litestl::alloc::New<AttachData>("coredcc::AttachDataCommand");
    impl->edit.held = held;
    impl->target = h;
    Result<Owned<Command>> cmd =
        own<Command>(rt, std::string(kAttachDataCommand) + "#impl", impl);
    if (!cmd.ok()) {
      return cmd.status();
    }
    Result<PropertyBag> out =
        ex->running() ?
            ex->runSub(ctx, std::move(cmd).value(), *def) :
            ex->runInstance(ctx, *def, std::move(cmd).value(), def->newInputs());
    return out.status();
  };
  Status s = run();
  if (!s.ok()) {
    held->destroy(); /* unless the command's discard did */
  }
  return s;
}

Status detail::registerAttachmentCommands(Runtime &rt)
{
  const auto def = [](const char *name, const char *ui) {
    CommandDef d(name);
    d.uiName(ui)
        .input(PropertyDef::pointerProp("block", StructId()))
        .input(PropertyDef::stringProp("type"));
    return d;
  };
  CDCC_TRY(addCommandOnce<EditCommand<true>>(rt, def(kAttachCommand, "Attach Data")));
  CDCC_TRY(addCommandOnce<EditCommand<false>>(rt, def(kDetachCommand, "Detach Data")));
  CommandDef data(kAttachDataCommand);
  data.uiName("Attach Data").flags(CommandFlags::Private);
  return addCommandOnce<AttachData>(rt, std::move(data));
}

} // namespace coredcc

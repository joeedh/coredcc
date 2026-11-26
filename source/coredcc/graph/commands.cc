#include "coredcc/graph/commands.h"

#include "coredcc/command/block_commands.h"
#include "coredcc/graph/provider.h"

#include "litestl/binding/binding.h"

#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;

namespace {

/* The block the `graph` input points into. */
BlockId graphInput(Context &ctx)
{
  const PropValue *v = ctx.inputs()->get("graph");
  const StableRef *ref = v ? v->tryPointer() : nullptr;
  return ref && ref->owner.kind == OwnerKey::Kind::Block ? ref->owner.block : BlockId();
}

int64_t intInput(Context &ctx, const char *name)
{
  return ctx.inputs()->get(name)->asInt();
}
std::string stringInput(Context &ctx, const char *name)
{
  return ctx.inputs()->get(name)->asString().c_str();
}
Status output(Context &ctx, const char *name, int64_t id)
{
  return ctx.outputs()->set(name, PropValue::ofInt(id));
}

/* A command that is one edit function. */
using EditFn = Status (*)(Context &, GraphEdit &);

template <EditFn Fn> struct GraphCommand {
  GraphEditRecord record;

  Status canRun(Context &ctx) const
  {
    const Library *lib = ctx.library();
    return lib && graphDefOf(*lib, lib->find(graphInput(ctx)))
               ? Status()
               : Status::error(StatusCode::FailedPrecondition, "graph: no such graph");
  }
  Status exec(Context &ctx)
  {
    Result<GraphEdit> edit = GraphEdit::begin(ctx, graphInput(ctx), record);
    if (!edit.ok()) {
      return edit.status();
    }
    Status s = Fn(ctx, *edit);
    if (!s.ok()) {
      CDCC_TRY(edit->revert());
    }
    return s;
  }
  void undo(Context &ctx)
  {
    record.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return record.redo(ctx);
  }
  uint64_t memSize() const
  {
    return record.memSize();
  }
  void discard()
  {
    record.discard();
  }
  static lbt::Struct<GraphCommand> *defineBindings()
  {
    auto *st =
        new lbt::Struct<GraphCommand>("coredcc::GraphCommand", sizeof(GraphCommand));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

struct CreateGraph {
  LibraryRef lib;
  CreatedBlocks created;

  Status canRun(Context &ctx) const
  {
    return ctx.library() ? Status()
                         : Status::error(StatusCode::FailedPrecondition,
                                         "the Context has no Library");
  }
  Status exec(Context &ctx)
  {
    Library &l = *ctx.library();
    lib = l.ref();
    Result<BlockHandle> h = newGraph(l, stringInput(ctx, "name"));
    if (!h.ok()) {
      return h.status();
    }
    created.add(l, *h);
    StableRef ref;
    ref.owner = OwnerKey::ofBlock(l.get(*h)->id());
    ref.type = StructId::of(kGraphDefType);
    return ctx.outputs()->set("graph", PropValue::ofPointer(ref));
  }
  void undo(Context &ctx)
  {
    Result<Library *> l = detail::libraryOf(ctx, lib);
    if (!l.ok()) {
      ctx.runtime().setStickyError(l.status());
      return;
    }
    created.undo(**l);
  }
  Status redo(Context &ctx)
  {
    Result<Library *> l = detail::libraryOf(ctx, lib);
    return l.ok() ? created.redo(**l) : l.status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + created.memSize();
  }
  void discard()
  {
    if (Library *l = lib.get()) {
      created.discard(*l);
    }
  }
  static lbt::Struct<CreateGraph> *defineBindings()
  {
    auto *st =
        new lbt::Struct<CreateGraph>("coredcc::CreateGraphCommand", sizeof(CreateGraph));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

Status addNode(Context &ctx, GraphEdit &edit)
{
  Result<NodeId> id = edit.addNode(stringInput(ctx, "type"));
  if (!id.ok()) {
    return id.status();
  }
  const PropValue &position = *ctx.inputs()->get("position");
  CDCC_TRY(edit.setPosition(*id, position.component(0), position.component(1)));
  const std::string label = stringInput(ctx, "label");
  if (!label.empty()) {
    CDCC_TRY(edit.setLabel(*id, label));
  }
  return output(ctx, "node", id->value);
}

Status removeNode(Context &ctx, GraphEdit &edit)
{
  return edit.removeNode(NodeId{intInput(ctx, "node")});
}

Status link(Context &ctx, GraphEdit &edit)
{
  const std::string from = stringInput(ctx, "fromSocket"),
                    to = stringInput(ctx, "toSocket");
  Result<LinkId> id =
      edit.link({NodeId{intInput(ctx, "from")}, from}, {NodeId{intInput(ctx, "to")}, to});
  return id.ok() ? output(ctx, "link", id->value) : id.status();
}

Status unlink(Context &ctx, GraphEdit &edit)
{
  return edit.unlink(LinkId{intInput(ctx, "link")});
}

Status addInterface(Context &ctx, GraphEdit &edit)
{
  const SocketSide side =
      ctx.inputs()->get("side")->asEnum() == 0 ? SocketSide::Input : SocketSide::Output;
  Result<int64_t> id = edit.addInterface(side,
                                         stringInput(ctx, "name"),
                                         stringInput(ctx, "type"),
                                         int(intInput(ctx, "index")));
  return id.ok() ? output(ctx, "socket", *id) : id.status();
}

Status removeInterface(Context &ctx, GraphEdit &edit)
{
  return edit.removeInterface(intInput(ctx, "socket"));
}

Status moveInterface(Context &ctx, GraphEdit &edit)
{
  return edit.moveInterface(intInput(ctx, "socket"), int(intInput(ctx, "index")));
}

Status setInterfaceType(Context &ctx, GraphEdit &edit)
{
  return edit.setInterfaceType(intInput(ctx, "socket"), stringInput(ctx, "type"));
}

CommandDef graphCommand(const char *id, const char *uiName)
{
  CommandDef def(id);
  def.uiName(uiName).input(
      PropertyDef::pointerProp("graph", StructId::of(kGraphDefType)));
  return def;
}

template <class Cmd> Status addOnce(Runtime &rt, CommandDef &def)
{
  if (rt.ext<CommandRegistry>().find(def.name().c_str())) {
    return Status();
  }
  return addNativeCommand<Cmd>(rt, std::move(def)).status();
}

} // namespace

Status registerGraphCommands(Runtime &rt)
{
  CDCC_TRY(registerGraphBlocks(rt));

  CommandDef create(kGraphCreateCommand);
  create.uiName("New Graph")
      .input(PropertyDef::stringProp("name").defaultValue(PropValue::ofString("Graph")))
      .output(PropertyDef::pointerProp("graph", StructId::of(kGraphDefType)));
  CDCC_TRY(addOnce<CreateGraph>(rt, create));

  CommandDef add = graphCommand(kGraphNodeAddCommand, "Add Node");
  add.input(PropertyDef::stringProp("type"))
      .input(PropertyDef::vecProp("position", 2))
      .input(PropertyDef::stringProp("label"))
      .output(PropertyDef::intProp("node"));
  CDCC_TRY(addOnce<GraphCommand<addNode>>(rt, add));

  CommandDef remove = graphCommand(kGraphNodeRemoveCommand, "Remove Node");
  remove.input(PropertyDef::intProp("node"));
  CDCC_TRY(addOnce<GraphCommand<removeNode>>(rt, remove));

  CommandDef linkDef = graphCommand(kGraphLinkCommand, "Link");
  linkDef.input(PropertyDef::intProp("from"))
      .input(PropertyDef::stringProp("fromSocket"))
      .input(PropertyDef::intProp("to"))
      .input(PropertyDef::stringProp("toSocket"))
      .output(PropertyDef::intProp("link"));
  CDCC_TRY(addOnce<GraphCommand<link>>(rt, linkDef));

  CommandDef unlinkDef = graphCommand(kGraphUnlinkCommand, "Unlink");
  unlinkDef.input(PropertyDef::intProp("link"));
  CDCC_TRY(addOnce<GraphCommand<unlink>>(rt, unlinkDef));

  CommandDef ifaceAdd = graphCommand(kGraphInterfaceAddCommand, "Add Interface Socket");
  ifaceAdd.input(PropertyDef::enumProp("side").item("INPUT", 0).item("OUTPUT", 1))
      .input(PropertyDef::stringProp("name"))
      .input(PropertyDef::stringProp("type").defaultValue(PropValue::ofString("float")))
      .input(PropertyDef::intProp("index")
                 .range(-1, 1 << 20)
                 .defaultValue(PropValue::ofInt(-1)))
      .output(PropertyDef::intProp("socket"));
  CDCC_TRY(addOnce<GraphCommand<addInterface>>(rt, ifaceAdd));

  CommandDef ifaceRemove =
      graphCommand(kGraphInterfaceRemoveCommand, "Remove Interface Socket");
  ifaceRemove.input(PropertyDef::intProp("socket"));
  CDCC_TRY(addOnce<GraphCommand<removeInterface>>(rt, ifaceRemove));

  CommandDef ifaceMove =
      graphCommand(kGraphInterfaceMoveCommand, "Move Interface Socket");
  ifaceMove.input(PropertyDef::intProp("socket")).input(PropertyDef::intProp("index"));
  CDCC_TRY(addOnce<GraphCommand<moveInterface>>(rt, ifaceMove));

  CommandDef ifaceType =
      graphCommand(kGraphInterfaceSetTypeCommand, "Set Interface Socket Type");
  ifaceType.input(PropertyDef::intProp("socket")).input(PropertyDef::stringProp("type"));
  return addOnce<GraphCommand<setInterfaceType>>(rt, ifaceType);
}

} // namespace coredcc

#include "coredcc/graph/ops.h"

#include "coredcc/base/text.h"
#include "coredcc/command/executor.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"

#include "litestl/binding/binding.h"

#include <cstring>
#include <string>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

/* ---- GraphOp ---- */

static Status insertAll(GraphDef &def, const NodeState &node, const Vector<LinkState, 1> &links)
{
  CDCC_TRY(def.restoreNode(node));
  for (const LinkState &l : links) {
    if (Status s = def.insertLink(l); !s.ok()) {
      def.removeNode(node.id).ignore(); /* and the links so far */
      return s;
    }
  }
  return Status();
}

Status GraphOp::apply(GraphDef &def, bool forward) const
{
  const NodeId n{id};
  const PropValue &value = forward ? after : before;
  switch (kind) {
    case Kind::Node:
      return forward != inverse ? insertAll(def, *node, links) : def.removeNode(node->id);
    case Kind::Link:
      return forward != inverse ? def.insertLink(links[0]) : def.unlink(links[0].id);
    case Kind::Label:
      return def.setLabel(n, value.asString().c_str());
    case Kind::Position:
      return def.setPosition(n, value.component(0), value.component(1));
    case Kind::Param:
    case Kind::Literal:
      return def.restoreValues(n, kind == Kind::Param, key.c_str(),
                               forward ? valuesAfter : valuesBefore);
    case Kind::Ref:
      return def.restoreRef(n, key.c_str(), forward ? blockAfter : blockBefore);
    case Kind::Interface:
      return forward != inverse ? def.restoreInterface(side, *socket, indexAfter) :
                                  def.removeInterface(id);
    case Kind::MoveInterface:
      return def.moveInterface(id, forward ? indexAfter : indexBefore);
    case Kind::InterfaceType:
      return forward ? def.setInterfaceType(id, after.asString().c_str()) :
                       def.restoreInterfaceType(id, *socket);
    case Kind::InterfaceName:
      return def.setInterfaceName(id, value.asString().c_str());
    case Kind::InterfaceDefault:
      return def.restoreInterfaceDefault(id, value);
  }
  return Status();
}

uint64_t GraphOp::memSize() const
{
  uint64_t n = sizeof(*this) + uint64_t(key.size());
  for (const PropValue *v : {&before, &after}) {
    n += v->type() == PropValue::Type::String ? uint64_t(v->asString().size()) : 0;
  }
  n += uint64_t(valuesBefore.size() + valuesAfter.size() + links.size()) * 128;
  if (node) {
    n += sizeof(NodeState) +
         uint64_t(node->params.size() + node->literals.size() + node->refs.size()) * 128;
  }
  return n + (socket ? sizeof(InterfaceSocket) : 0);
}

/* ---- GraphEditRecord ---- */

Result<GraphDef *> GraphEditRecord::def() const
{
  const Library *lib = block_.library();
  GraphDef *def = lib ? graphDefOf(*lib, block_.get()) : nullptr;
  if (!def) {
    return Status::error(StatusCode::FailedPrecondition, "the edited graph isn't there");
  }
  return def;
}

void GraphEditRecord::pin(Library &lib, BlockId id)
{
  const BlockHandle h = lib.find(id);
  for (const LibraryPin &p : pins_) {
    if (p.handle() == h) {
      return;
    }
  }
  if (h) {
    pins_.append(LibraryPin(lib, h, PinKind::History));
  }
}

Status GraphEditRecord::revert(GraphDef &def, int from)
{
  for (int i = from - 1; i >= 0; i--) {
    CDCC_TRY(ops_[i].apply(def, false));
  }
  return Status();
}

void GraphEditRecord::undo(Context &ctx)
{
  Result<GraphDef *> d = def();
  Status s = d.ok() ? revert(**d, int(ops_.size())) : d.status();
  if (!s.ok()) {
    ctx.runtime().setStickyError(std::move(s));
  }
}

Status GraphEditRecord::redo(Context &ctx)
{
  (void)ctx;
  Result<GraphDef *> d = def();
  if (!d.ok()) {
    return d.status();
  }
  for (int i = 0; i < int(ops_.size()); i++) {
    if (Status s = ops_[i].apply(**d, true); !s.ok()) {
      revert(**d, i).ignore();
      return s;
    }
  }
  return Status();
}

void GraphEditRecord::discard()
{
  ops_.clear();
  pins_.clear();
  block_.release();
}

uint64_t GraphEditRecord::memSize() const
{
  uint64_t n = sizeof(*this) + uint64_t(pins_.size()) * sizeof(LibraryPin);
  for (const GraphOp &op : ops_) {
    n += op.memSize();
  }
  return n;
}

/* ---- GraphEdit ---- */

Result<GraphEdit> GraphEdit::begin(Context &ctx, BlockId id, GraphEditRecord &record)
{
  Library *lib = ctx.library();
  if (!lib) {
    return Status::error(StatusCode::FailedPrecondition, "the Context has no Library");
  }
  const BlockHandle h = lib->find(id);
  GraphDef *def = graphDefOf(*lib, h);
  if (!def) {
    return Status::error(StatusCode::FailedPrecondition, "no such graph");
  }
  record.block_ = HistoryBlock(*lib, h);
  return GraphEdit(def, lib, &record);
}

Status GraphEdit::revert()
{
  Status s = record_->revert(*def_, int(record_->ops_.size()));
  record_->ops_.clear();
  return s;
}

void GraphEdit::push(GraphOp op)
{
  record_->pin(*lib_, op.blockBefore);
  record_->pin(*lib_, op.blockAfter);
  if (op.node) {
    record_->pin(*lib_, op.node->group);
    for (const NodeState::Ref &r : op.node->refs) {
      record_->pin(*lib_, r.target);
    }
  }
  record_->ops_.append(std::move(op));
}

Result<NodeId> GraphEdit::addNode(std::string_view type)
{
  Result<NodeId> id = def_->addNode(type);
  if (id.ok()) {
    GraphOp op;
    op.node = std::make_unique<NodeState>(def_->stateOf(*def_->node(*id)));
    push(std::move(op));
  }
  return id;
}

Status GraphEdit::insertNode(const NodeState &state)
{
  CDCC_TRY(def_->insertNode(state));
  GraphOp op;
  op.node = std::make_unique<NodeState>(def_->stateOf(*def_->node(state.id)));
  push(std::move(op));
  return Status();
}

Status GraphEdit::removeNode(NodeId id)
{
  GraphOp op;
  op.inverse = true;
  if (const GraphNode *n = def_->node(id)) {
    op.node = std::make_unique<NodeState>(def_->stateOf(*n));
    for (const GraphLink *l : n->linksIn()) {
      op.links.append(GraphDef::stateOf(*l));
    }
    for (const GraphLink *l : n->linksOut()) {
      if (l->to != n) { /* a link to itself is among the inputs' */
        op.links.append(GraphDef::stateOf(*l));
      }
    }
  }
  CDCC_TRY(def_->removeNode(id));
  push(std::move(op));
  return Status();
}

Status GraphEdit::setLabel(NodeId id, std::string_view label)
{
  GraphOp op;
  op.kind = GraphOp::Kind::Label;
  op.id = id.value;
  if (const GraphNode *n = def_->node(id)) {
    op.before = PropValue::ofString(n->label().c_str());
  }
  op.after = PropValue::ofString(label);
  CDCC_TRY(def_->setLabel(id, label));
  push(std::move(op));
  return Status();
}

static PropValue vec2(double x, double y)
{
  const double c[2] = {x, y};
  return PropValue::ofComponents(PropValue::Type::Vec2, c);
}

Status GraphEdit::setPosition(NodeId id, double x, double y)
{
  GraphOp op;
  op.kind = GraphOp::Kind::Position;
  op.id = id.value;
  if (const GraphNode *n = def_->node(id)) {
    op.before = vec2(n->x(), n->y());
  }
  op.after = vec2(x, y);
  CDCC_TRY(def_->setPosition(id, x, y));
  push(std::move(op));
  return Status();
}

/* The stored values of one name. */
static NodeValues named(const NodeValues &list, std::string_view name)
{
  NodeValues out;
  for (const NodeValue &v : list) {
    if (name == v.name.c_str()) {
      out.append(v);
    }
  }
  return out;
}

Status GraphEdit::setParam(NodeId id, std::string_view name, const PropValue &value)
{
  GraphOp op;
  op.id = id.value;
  op.key = lsString(name);
  const GraphNode *n = def_->node(id);
  const NodeType *type = n ? n->type() : nullptr;
  const int i = type ? type->paramIndex(name) : -1;
  if (i >= 0 && type->params[i].def.kind() == PropKind::Pointer) {
    op.kind = GraphOp::Kind::Ref;
    op.blockBefore = n->blockTarget(name);
    CDCC_TRY(def_->setParam(id, name, value));
    op.blockAfter = n->blockTarget(name);
  }
  else {
    op.kind = GraphOp::Kind::Param;
    if (n) {
      op.valuesBefore = named(n->params(), name);
    }
    CDCC_TRY(def_->setParam(id, name, value));
    op.valuesAfter = named(n->params(), name);
  }
  push(std::move(op));
  return Status();
}

template <class Fn> Status GraphEdit::literalEdit(NodeId id, std::string_view input, Fn &&fn)
{
  GraphOp op;
  op.kind = GraphOp::Kind::Literal;
  op.id = id.value;
  op.key = lsString(input);
  const GraphNode *n = def_->node(id);
  if (n) {
    op.valuesBefore = named(n->literals(), input);
  }
  CDCC_TRY(fn());
  op.valuesAfter = named(n->literals(), input);
  push(std::move(op));
  return Status();
}

Status GraphEdit::setLiteral(NodeId id, std::string_view input, const PropValue &value)
{
  return literalEdit(id, input, [&]() { return def_->setLiteral(id, input, value); });
}

Status GraphEdit::useLiteral(NodeId id, std::string_view input, bool on)
{
  return literalEdit(id, input, [&]() { return def_->useLiteral(id, input, on); });
}

Status GraphEdit::setGroup(NodeId id, BlockId block)
{
  GraphOp op;
  op.kind = GraphOp::Kind::Ref;
  op.id = id.value;
  if (const GraphNode *n = def_->node(id)) {
    op.blockBefore = n->group().target;
  }
  CDCC_TRY(def_->setGroup(id, block));
  op.blockAfter = def_->node(id)->group().target;
  push(std::move(op));
  return Status();
}

Result<LinkId> GraphEdit::link(SocketRef from, SocketRef to)
{
  const GraphNode *n = def_->node(to.node);
  const GraphLink *old = n ? n->linkInto(to.key) : nullptr;
  GraphOp removed;
  removed.kind = GraphOp::Kind::Link;
  removed.inverse = true;
  if (old) {
    removed.links.append(GraphDef::stateOf(*old));
  }
  Result<LinkId> id = def_->link(from, to);
  if (!id.ok()) {
    return id;
  }
  if (old) {
    push(std::move(removed));
  }
  GraphOp op;
  op.kind = GraphOp::Kind::Link;
  op.links.append(GraphDef::stateOf(*def_->link(*id)));
  push(std::move(op));
  return id;
}

Status GraphEdit::insertLink(const LinkState &state)
{
  CDCC_TRY(def_->insertLink(state));
  GraphOp op;
  op.kind = GraphOp::Kind::Link;
  op.links.append(state);
  push(std::move(op));
  return Status();
}

Status GraphEdit::unlink(LinkId id)
{
  GraphOp op;
  op.kind = GraphOp::Kind::Link;
  op.inverse = true;
  if (const GraphLink *l = def_->link(id)) {
    op.links.append(GraphDef::stateOf(*l));
  }
  CDCC_TRY(def_->unlink(id));
  push(std::move(op));
  return Status();
}

/* ---- interface ---- */

/* Where a socket is. */
static int indexOf(const GraphDef &def, int64_t id, SocketSide &side)
{
  for (SocketSide s : {SocketSide::Input, SocketSide::Output}) {
    const Vector<InterfaceSocket> &list = def.interfaceSockets(s);
    for (int i = 0; i < int(list.size()); i++) {
      if (list[i].id == id) {
        side = s;
        return i;
      }
    }
  }
  return -1;
}

Result<int64_t> GraphEdit::addInterface(SocketSide side,
                                        std::string_view name,
                                        std::string_view socketType,
                                        int index)
{
  Result<int64_t> id = def_->addInterface(side, name, socketType, index);
  if (!id.ok()) {
    return id;
  }
  GraphOp op;
  op.kind = GraphOp::Kind::Interface;
  op.id = *id;
  op.indexAfter = indexOf(*def_, *id, op.side);
  op.socket = std::make_unique<InterfaceSocket>(*def_->interfaceSocket(*id));
  push(std::move(op));
  return id;
}

Status GraphEdit::removeInterface(int64_t id)
{
  GraphOp op;
  op.kind = GraphOp::Kind::Interface;
  op.inverse = true;
  op.id = id;
  op.indexAfter = indexOf(*def_, id, op.side);
  if (const InterfaceSocket *s = def_->interfaceSocket(id)) {
    op.socket = std::make_unique<InterfaceSocket>(*s);
  }
  CDCC_TRY(def_->removeInterface(id));
  push(std::move(op));
  return Status();
}

Status GraphEdit::moveInterface(int64_t id, int index)
{
  GraphOp op;
  op.kind = GraphOp::Kind::MoveInterface;
  op.id = id;
  op.indexBefore = indexOf(*def_, id, op.side);
  op.indexAfter = index;
  CDCC_TRY(def_->moveInterface(id, index));
  push(std::move(op));
  return Status();
}

Status GraphEdit::setInterfaceType(int64_t id, std::string_view socketType)
{
  GraphOp op;
  op.kind = GraphOp::Kind::InterfaceType;
  op.id = id;
  if (const InterfaceSocket *s = def_->interfaceSocket(id)) {
    op.socket = std::make_unique<InterfaceSocket>(*s);
  }
  op.after = PropValue::ofString(socketType);
  CDCC_TRY(def_->setInterfaceType(id, socketType));
  push(std::move(op));
  return Status();
}

Status GraphEdit::setInterfaceName(int64_t id, std::string_view name)
{
  GraphOp op;
  op.kind = GraphOp::Kind::InterfaceName;
  op.id = id;
  if (const InterfaceSocket *s = def_->interfaceSocket(id)) {
    op.before = PropValue::ofString(s->name.c_str());
  }
  op.after = PropValue::ofString(name);
  CDCC_TRY(def_->setInterfaceName(id, name));
  push(std::move(op));
  return Status();
}

Status GraphEdit::setInterfaceDefault(int64_t id, const PropValue &value)
{
  GraphOp op;
  op.kind = GraphOp::Kind::InterfaceDefault;
  op.id = id;
  if (const InterfaceSocket *s = def_->interfaceSocket(id)) {
    op.before = s->defaultValue;
  }
  CDCC_TRY(def_->setInterfaceDefault(id, value));
  op.after = def_->interfaceSocket(id)->defaultValue;
  push(std::move(op));
  return Status();
}

/* ---- runGraphEdit ---- */

namespace {

constexpr const char *kEditCommand = "graph.edit";
constexpr const char *kEditImpl = "graph.edit#impl";

struct RunGraphEdit {
  BlockId graph;
  GraphEditFn fn;
  GraphEditRecord record;

  Status canRun(Context &) const
  {
    return fn ? Status() :
                Status::error(StatusCode::FailedPrecondition, "graph.edit: use runGraphEdit");
  }
  Status exec(Context &ctx)
  {
    Result<GraphEdit> edit = GraphEdit::begin(ctx, graph, record);
    if (!edit.ok()) {
      return edit.status();
    }
    Status s = fn(*edit);
    fn = GraphEditFn(); /* what it captured is the caller's */
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
  static lbt::Struct<RunGraphEdit> *defineBindings()
  {
    auto *st = new lbt::Struct<RunGraphEdit>("coredcc::GraphEditCommand", sizeof(RunGraphEdit));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

Result<const CommandDef *> editDef(Runtime &rt)
{
  if (const CommandDef *d = rt.ext<CommandRegistry>().find(kEditCommand)) {
    if (d->implType() != TypeId::of(kEditImpl)) {
      return Status::error(StatusCode::AlreadyExists,
                           "graph.edit is registered with another implementation");
    }
    return d;
  }
  CommandDef def(kEditCommand);
  def.uiName("Edit Graph").flags(CommandFlags::Private).input(PropertyDef::stringProp("name"));
  return addNativeCommand<RunGraphEdit>(rt, std::move(def));
}

} // namespace

Status runGraphEdit(Context &ctx, BlockId graph, std::string_view name, GraphEditFn fn)
{
  CommandExecutor *ex = ctx.executor();
  if (!ex) {
    return Status::error(StatusCode::FailedPrecondition,
                         "runGraphEdit: the Context has no executor");
  }
  Result<const CommandDef *> def = editDef(ctx.runtime());
  if (!def.ok()) {
    return def.status();
  }
  RunGraphEdit *impl = litestl::alloc::New<RunGraphEdit>("coredcc::GraphEditCommand");
  impl->graph = graph;
  impl->fn = std::move(fn);
  Result<Owned<Command>> cmd = own<Command>(ctx.runtime(), kEditImpl, impl);
  if (!cmd.ok()) {
    return cmd.status();
  }
  PropertyBag in = (*def)->newInputs();
  CDCC_TRY(in.set("name", PropValue::ofString(name)));
  Result<PropertyBag> out =
      ex->running() ? ex->runSub(ctx, std::move(cmd).value(), **def) :
                      ex->runInstance(ctx, **def, std::move(cmd).value(), std::move(in));
  return out.ok() ? Status() : out.status();
}

} // namespace coredcc

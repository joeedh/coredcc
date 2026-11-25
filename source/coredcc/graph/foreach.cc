/* graph.foreach: its tables, and how the evaluator runs their elements. */
#include "coredcc/graph/foreach.h"

#include "coredcc/graph/evaluator.h"
#include "coredcc/graph/socket_type.h"
#include "coredcc/lib/library.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::Map;
using litestl::util::Vector;

/* ---- tables ---- */

PropertyDef foreachMaxCachedParam()
{
  PropertyDef def = PropertyDef::intProp(kMaxCachedParam);
  def.uiName("Cached Elements").range(0, 1 << 24).defaultValue(PropValue::ofInt(4096));
  return def;
}

const Elements::Column *Elements::column(std::string_view name) const
{
  for (const Column &c : columns) {
    if (name == c.name.c_str()) {
      return &c;
    }
  }
  return nullptr;
}

Status Elements::validate(bool checkKeys) const
{
  auto invalid = [](const std::string &text) {
    return Status::error(StatusCode::InvalidArgument, text.c_str());
  };
  for (const Column &c : columns) {
    const std::string name = std::string("column '") + c.name.c_str() + "'";
    if (column(c.name.c_str()) != &c) {
      return invalid("the table has two of " + name);
    }
    if (c.values.size() != keys.size()) {
      return invalid(name + " has " + std::to_string(c.values.size()) + " values for " +
                     std::to_string(keys.size()) + " keys");
    }
  }
  Map<int64_t, bool> seen;
  for (int i = 0; checkKeys && i < size(); i++) {
    const int64_t key = keys[i];
    if (!seen.add(key, true)) {
      return invalid("the key " + std::to_string(key) + " is in the table twice");
    }
  }
  return Status();
}

bool Elements::operator==(const Elements &b) const
{
  if (keys.size() != b.keys.size() || columns.size() != b.columns.size() ||
      !std::equal(keys.begin(), keys.end(), b.keys.begin()))
  {
    return false;
  }
  for (int i = 0; i < int(columns.size()); i++) {
    const Column &x = columns[i], &y = b.columns[i];
    if (strcmp(x.name.c_str(), y.name.c_str()) != 0 ||
        x.values.size() != y.values.size() ||
        !std::equal(x.values.begin(), x.values.end(), y.values.begin()))
    {
      return false;
    }
  }
  return true;
}

size_t PayloadTraits<Elements>::bytes(const Elements &e)
{
  size_t n = sizeof(e) + size_t(e.keys.size()) * sizeof(int64_t);
  for (const Elements::Column &c : e.columns) {
    n += sizeof(c) + size_t(c.values.size()) * sizeof(GraphValue);
  }
  return n;
}

namespace graph {

Element::Element(int64_t key, ElementCounts *counts) : key(key), counts(counts)
{
  if (counts) {
    counts->elements++;
  }
}

Element::~Element()
{
  if (counts) {
    counts->elements--;
  }
  litestl::alloc::Delete(frame);
}

Foreach::~Foreach()
{
  clear();
}

void Foreach::clear()
{
  for (auto &pair : cached) {
    litestl::alloc::Delete(pair.value);
  }
  cached = {};
  if (shared) {
    litestl::alloc::Delete(shared);
    shared = nullptr;
  }
  madeFrom = PayloadRef();
}

/** Which columns of a table are for which interface inputs. */
struct ElementsMap {
  struct Input {
    int64_t id = 0;
    const Elements::Column *column = nullptr;
    /** In the table the last output was made from. */
    const Elements::Column *oldColumn = nullptr;
    const SocketType *type = nullptr;
  };
  litestl::util::Vector<Input, 4> inputs;
  /** Whether an element's inputs are the same in a row of each. */
  bool sameRow(int row, int oldRow) const;
};

/* ---- when it runs ---- */

void Evaluator::computeForeach(Frame &f, int node)
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  KernelState &k = kernel(f, node);
  Foreach &s = *k.foreach;
  Vector<TraceInput, 2> inputs;
  for (int i = 0; i < n.inCount; i++) {
    const InputState &in = f.inputs[n.inBegin + i];
    if (in.error) {
      return settle(k, in.error, true);
    }
    inputs.append(in.origin);
  }
  GraphDef *def = f.def->groupTarget(*n.node);
  if (Status status = checkGroup(f, *n.node, def); !status.ok()) {
    return settle(k, makeError(f, n.node->id(), std::move(status)), false);
  }
  if (s.defSerial != def->serial()) {
    s.clear();
    s.defSerial = def->serial(); /* worldMoved sees it too */
  }
  /* The state is noted before the elements run, so what moves meanwhile is seen next
   * time. */
  bool world = worldMoved(f, node, k, *def);
  world |= k.trace.inputs.size() != inputs.size() ||
           !std::equal(inputs.begin() + 1, inputs.end(), k.trace.inputs.begin() + 1);
  const bool table = !k.ran || !(k.trace.inputs[0] == inputs[0]);
  k.trace.inputs = std::move(inputs);
  if (world) {
    s.world = rt_.nextVersion();
    s.deps.clear();
    k.trace.more = nullptr;
  }
  if (world || table) {
    const auto start = std::chrono::steady_clock::now();
    k.runs++;
    k.ran = false;
    ErrorPtr error = runElements(f, node, k, *def, table);
    if (overBudget_) {
      return;
    }
    if (f.deps) {
      f.deps->setBlocks(k, rt_.ext<ClientRegistry>());
    }
    k.ran = true;
    k.runError = std::move(error);
    k.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    k.totalSeconds += k.seconds;
  }
  if (sink_.set) {
    sink_.set->add(k.trace, *sink_.trace);
  }
  settle(k, k.runError, false);
}

bool Evaluator::worldMoved(Frame &f, int node, KernelState &k, const GraphDef &def)
{
  Foreach &s = *k.foreach;
  bool moved = !k.ran;
  auto note = [&moved](uint64_t &was, uint64_t now) {
    moved |= was != now;
    was = now;
  };
  note(k.trace.paramVersion, f.topo->nodes[node].node->paramVersion());
  note(s.nodeTypes, rt_.ext<NodeTypeRegistry>().stamp());
  note(s.socketTypes, rt_.ext<SocketTypeRegistry>().stamp());

  Vector<const GraphDef *> list;
  Vector<uint64_t> defs;
  Map<const GraphDef *, bool> seen;
  seen.add(&def, true);
  list.append(&def);
  for (int i = 0; i < int(list.size()); i++) {
    const GraphDef &d = *list[i];
    defs.append(d.contentVersion()); /* unique, so it says which def, too */
    const std::shared_ptr<const GraphTopology> topo = d.topology();
    defs.append(topo->serial); /* groups' targets, also lost ones, and interior sockets */
    for (const GraphTopology::Group &g : topo->groups) {
      const GraphDef *target = d.groupTarget(*g.node);
      if (target && seen.add(target, true)) {
        list.append(target);
      }
    }
  }
  if (defs.size() != s.defs.size() ||
      !std::equal(defs.begin(), defs.end(), s.defs.begin()))
  {
    moved = true;
    s.defs = std::move(defs);
    s.serials.clear();
    for (const GraphDef *d : list) {
      s.serials.add(d->serial(), true);
    }
  }

  /* The Library may write a Pointer param's cell, which moves its epoch but not the def.
   */
  const uint64_t epoch = lib_.get() ? lib_.get()->epoch() : 0;
  if (s.epoch != epoch) {
    s.epoch = epoch;
    Vector<BlockId> refs;
    for (const GraphDef *d : list) {
      d->forEachNode([&](const GraphNode &n) {
        for (const NodeBlockRef &r : n.blockRefs()) {
          refs.append(r.cell.target);
        }
      });
    }
    if (refs.size() != s.refs.size() ||
        !std::equal(refs.begin(), refs.end(), s.refs.begin()))
    {
      moved = true;
      s.refs = std::move(refs);
    }
  }
  return moved || !blocks_.current(k.trace);
}

/* ---- running ---- */

static ErrorPtr inElement(const GraphError &e, int64_t key)
{
  GraphError out = e;
  out.status = Status::error(
      e.status.code(),
      ("element " + std::to_string(key) + ": " + e.status.message()).c_str());
  out.origin.elements.clear();
  out.origin.elements.append(key);
  for (int64_t inner : e.origin.elements) {
    out.origin.elements.append(inner);
  }
  return std::make_shared<const GraphError>(std::move(out));
}

ErrorPtr
Evaluator::runElements(Frame &f, int node, KernelState &k, GraphDef &def, bool newTable)
{
  const GraphTopology::Node &n = f.topo->nodes[node];
  Foreach &s = *k.foreach;
  const PayloadRef ref = f.inputs[n.inBegin].value->asPayload();
  const Elements none;
  const Elements *table = ref ? ref.get<Elements>() : &none;
  auto fail = [&](Status status) {
    return makeError(f, n.node->id(), std::move(status), kElementsSocket);
  };
  if (!table) {
    return fail(Status::error(StatusCode::FailedPrecondition,
                              "the table's blocks aren't there any more"));
  }
  const Elements *old = s.madeIn == s.world ? s.madeFrom.get<Elements>() : nullptr;
  const bool sameKeys =
      old && old->keys.size() == table->keys.size() &&
      std::equal(old->keys.begin(), old->keys.end(), table->keys.begin());
  if (newTable) {
    s.table = table->validate(!sameKeys);
  }
  if (!s.table.ok()) {
    return fail(s.table);
  }

  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  ElementsMap map;
  for (const InterfaceSocket &in : def.interfaceSockets(SocketSide::Input)) {
    map.inputs.append(ElementsMap::Input{in.id,
                                         table->column(in.name.c_str()),
                                         old ? old->column(in.name.c_str()) : nullptr,
                                         types.find(in.typeName.c_str())});
  }
  /* For the same keys it starts as a copy of the last one, made when the first value
   * differs. */
  Elements out;
  const Elements *oldOut = old ? k.outputs[0].value.payload<Elements>() : nullptr;
  bool changed = !sameKeys;
  if (!sameKeys) {
    out.keys = table->keys;
    for (const InterfaceSocket &o : def.interfaceSockets(SocketSide::Output)) {
      Elements::Column c;
      c.name = o.name;
      c.values.resize(size_t(table->size()));
      out.columns.append(std::move(c));
    }
  }
  const bool marks =
      !sameKeys || s.failed; /* else the elements there are, are the table's */
  Map<int64_t, int> oldRows;
  for (int i = 0; old && !sameKeys && i < old->size(); i++) {
    oldRows.add(old->keys[i], i);
  }
  auto newElement = [&](int64_t key, ElementCounts *counts) {
    Element *e = litestl::alloc::New<Element>("coredcc::GraphElement", key, counts);
    e->frame = newFrame(f, *n.node, &def);
    e->frame->top = e->frame;
    e->frame->element = e;
    e->frame->counts = counts;
    return e;
  };
  auto evict = [&](auto &&gone) {
    Vector<Element *> list;
    for (auto &pair : s.cached) {
      if (gone(*pair.value)) {
        list.append(pair.value);
      }
    }
    for (Element *e : list) {
      s.cached.remove(e->key);
      litestl::alloc::Delete(e);
    }
  };

  const int64_t maxCached = f.def->paramValue(*n.node, kMaxCachedParam).asInt();
  s.failed = true; /* until it is through */
  ErrorPtr error;
  for (int row = 0; row < table->size() && !error; row++) {
    const int64_t key = table->keys[row];
    const int *was = old && !sameKeys ? oldRows.lookup_ptr(key) : nullptr;
    const int oldRow = sameKeys ? row : was ? *was : -1;
    if (oldRow >= 0 && map.sameRow(row, oldRow)) {
      for (int i = 0; !sameKeys && i < int(out.columns.size()); i++) {
        out.columns[i].values[row] = oldOut->columns[i].values[oldRow];
      }
      Element **kept = marks && row < maxCached ? s.cached.lookup_ptr(key) : nullptr;
      if (kept) {
        (*kept)->run = k.runs;
      }
      continue;
    }
    Element **found = row < maxCached ? s.cached.lookup_ptr(key) : nullptr;
    Element *e = found ? *found : nullptr;
    if (!found && row < maxCached && elements_.elements < maxFrames &&
        elements_.kernels < maxCachedKernels)
    {
      e = newElement(key, &elements_);
      s.cached.add(key, e);
    }
    if (!e) {
      s.shared = s.shared ? s.shared : newElement(0, nullptr);
      e = s.shared;
      e->key = key;
    }
    e->run = k.runs;
    bool same = true;
    if (Status status = setRow(*e, map, row, same); !status.ok()) {
      error = fail(std::move(status));
    } else if (!same || e->world != s.world) {
      error = runElement(f, node, k, *e);
      if (overBudget_) {
        return nullptr;
      }
    }
    if (error) {
      error = inElement(*error, key);
      e->world = 0;
      continue;
    }
    e->world = s.world;
    for (int i = 0; i < int(e->outputs.size()); i++) {
      if (!changed && oldOut->columns[i].values[row] != e->outputs[i]) {
        changed = true;
        out = *oldOut;
      }
      if (changed) {
        out.columns[i].values[row] = e->outputs[i];
      }
    }
  }
  s.rows = table->size();
  s.failed = error != nullptr;
  if (error) {
    s.cachedRows = int(s.cached.size());
    /* Elements that are gone are kept for when they are back, but not without limit. */
    if (int64_t(s.cached.size()) > 2 * maxCached || elements_.elements >= maxFrames ||
        elements_.kernels >= maxCachedKernels)
    {
      Map<int64_t, bool> wanted;
      for (int row = 0; row < table->size() && row < maxCached; row++) {
        wanted.add(table->keys[row], true);
      }
      evict([&](const Element &e) { return !wanted.lookup_ptr(e.key); });
    }
    return error;
  }
  if (marks) {
    evict([&](const Element &e) { return e.run != k.runs; });
  }
  s.cachedRows = int(s.cached.size());
  if (changed) {
    GraphValue v = GraphValue::ofPayload(makePayload(std::move(out)));
    if (sameKeys || k.outputs[0].value != v) {
      k.outputs[0].value = std::move(v);
      k.outputs[0].changedAt = rt_.nextVersion();
    }
  }
  s.madeFrom = ref;
  s.madeIn = s.world;
  return nullptr;
}

bool ElementsMap::sameRow(int row, int oldRow) const
{
  for (const Input &in : inputs) {
    if (!in.column != !in.oldColumn ||
        (in.column && in.column->values[row] != in.oldColumn->values[oldRow]))
    {
      return false;
    }
  }
  return true;
}

Status Evaluator::setRow(Element &e, const ElementsMap &map, int row, bool &same)
{
  const SocketTypeRegistry &types = rt_.ext<SocketTypeRegistry>();
  auto sameId = [](const ElementsMap::Input &m, const Element::Input &in) {
    return m.id == in.id;
  };
  if (e.inputs.size() != map.inputs.size() ||
      !std::equal(map.inputs.begin(), map.inputs.end(), e.inputs.begin(), sameId))
  {
    Vector<Element::Input, 1> old = std::move(e.inputs);
    e.inputs = {};
    e.inputs.resize(size_t(map.inputs.size()));
    for (int i = 0; i < int(map.inputs.size()); i++) {
      for (Element::Input &in : old) {
        if (sameId(map.inputs[i], in)) {
          e.inputs[i] = std::move(in);
        }
      }
      e.inputs[i].id = map.inputs[i].id;
    }
  }
  for (int i = 0; i < int(map.inputs.size()); i++) {
    const ElementsMap::Input &m = map.inputs[i];
    Element::Input &in = e.inputs[i];
    if (!m.column) {
      same &= !in.column;
      in.column = false;
      in.value = GraphValue();
      continue;
    }
    const GraphValue *v = &m.column->values[row];
    GraphValue converted;
    if (!m.type->holds(*v)) { /* the type is known: the node's own input is of it */
      const SocketType *from = types.typeOf(*v);
      Status status = from ? types.convert(*from, *m.type, *v, converted)
                           : Status::error(StatusCode::TypeMismatch, "there is no value");
      if (!status.ok()) {
        same = false;
        return Status::error(
            status.code(),
            (std::string("column '") + m.column->name.c_str() + "': " + status.message())
                .c_str());
      }
      v = &converted;
    }
    if (in.value != *v) { /* None without a column */
      same = false;
      in.column = true;
      in.value = *v;
      in.version = rt_.nextVersion();
    }
  }
  return Status();
}

const GraphValue *Evaluator::elementInput(const Frame &f,
                                          int interfaceIndex,
                                          const SocketType &type,
                                          TraceInput &origin)
{
  const Element::Input &in = f.element->inputs[interfaceIndex];
  if (in.column) {
    Origin o;
    o.kind = Origin::Kind::Column;
    o.version = in.version; /* unique */
    origin = TraceInput{o, chains_.extend(TypeChains::kEmpty, &type)};
    return &in.value;
  }
  /* The node's own inputs, all computed and without errors. `elements` is the first. */
  const GraphTopology::Node &n = f.parent->topo->nodes[f.groupIndex];
  const InputState &s = f.parent->inputs[n.inBegin + 1 + interfaceIndex];
  origin = s.origin;
  return s.value;
}

ErrorPtr Evaluator::runElement(Frame &f, int node, KernelState &k, Element &e)
{
  Frame &ef = *e.frame;
  Foreach &s = *k.foreach;
  const uint64_t pass = pass_;
  const int numbers = numbers_;
  const Sink sink = sink_;
  pass_ = ++passes_;
  sink_ = Sink{&s.deps, &k.trace};
  ef.groupIndex = node;
  if (s.pinned == call_) {
    ef.pinned = call_; /* one pin for all elements */
  }
  s.pinned = call_;
  pinDef(ef);
  sync(ef);

  const Vector<InterfaceSocket> &outs = ef.def->interfaceSockets(SocketSide::Output);
  e.outputs.resize(size_t(outs.size()));
  ErrorPtr error;
  for (int i = 0; i < int(outs.size()) && !error; i++) {
    if (ef.topo->outputNode < 0) {
      if (const SocketType *t =
              rt_.ext<SocketTypeRegistry>().find(outs[i].typeName.c_str()))
      {
        e.outputs[i] = t->defaultValue;
      } else {
        error = makeError(f, ef.groupNode, unknownType());
      }
      continue;
    }
    const Vertex v{&ef, ef.topo->nodes[ef.topo->outputNode].inBegin + i, false};
    if (!walk(v)) {
      break; /* over budget */
    }
    const InputState &in = ef.inputs[v.index];
    error = in.error;
    if (!error) {
      e.outputs[i] = *in.value;
    }
  }
  pass_ = pass;
  numbers_ = numbers;
  sink_ = sink;
  return error;
}

} // namespace graph
} // namespace coredcc

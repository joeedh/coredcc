#pragma once

/* A graph with a foreach node over a small def, for the foreach tests. */

#include "graph_fixture.h"

#include "coredcc/graph/foreach.h"
#include "coredcc/graph/instance.h"

#include <initializer_list>
#include <vector>

namespace fixture {

/* ---- tables ---- */

struct Column {
  const char *name;
  std::vector<double> values;
};
using Columns = std::vector<Column>;

inline Elements table(std::initializer_list<int64_t> keys, const Columns &columns)
{
  Elements t;
  for (int64_t key : keys) {
    t.keys.append(key);
  }
  for (const Column &c : columns) {
    Elements::Column out;
    out.name = c.name;
    for (double v : c.values) {
      out.values.append(GraphValue::ofFloat(v));
    }
    t.columns.append(std::move(out));
  }
  return t;
}

inline GraphValue tableValue(Elements t)
{
  return GraphValue::ofPayload(makePayload(std::move(t)));
}

/* ---- kernels ---- */

/* An interface things may have. */
struct Scaled : IfaceRef {
  double scale() const
  {
    return CDCC_CALL(scale);
  }
  CDCC_INTERFACE(Scaled, "test::Scaled")
  {
    BIND_STRUCT_METHOD(st, scale, MARGS());
  }
};

struct ThingScaled {
  double value = 0;
  double scale() const
  {
    return value;
  }
  static lbt::Struct<ThingScaled> *defineBindings()
  {
    auto *st = new lbt::Struct<ThingScaled>("test::ThingScaled", sizeof(ThingScaled));
    BIND_STRUCT_METHOD(st, scale, MARGS());
    return st;
  }
};

/* a + b, but fails when a is -100 or less. */
inline Status sumKernel(NodeExecContext &ctx)
{
  const double a = ctx.input("a")->asFloat();
  if (a <= -100) {
    return Status::error(StatusCode::OutOfRange,
                         ("a is " + std::to_string(int(a))).c_str());
  }
  return ctx.setOutput("value", GraphValue::ofFloat(a + ctx.input("b")->asFloat()));
}

/* The value of its thing: -2 if it isn't there, -3 if it isn't Live. userData may name a
 * block to notify, once. */
inline Status readKernel(NodeExecContext &ctx)
{
  const BlockUse use = ctx.paramBlock("thing");
  const Thing *t = static_cast<const Thing *>(use.data);
  if (BlockId *id = static_cast<BlockId *>(ctx.userData()); id && !id->isNull()) {
    const BlockId notify = *id;
    *id = BlockId();
    ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(notify));
  }
  return ctx.setOutput(0, GraphValue::ofFloat(t ? t->value : (use.handle ? -3 : -2)));
}

/* The same if `a` isn't negative, else 0 without reading it. */
inline Status readIfKernel(NodeExecContext &ctx)
{
  return ctx.input(0).asFloat() < 0 ? ctx.setOutput(0, GraphValue::ofFloat(0))
                                    : readKernel(ctx);
}

/* What the interface of its thing says, or -1. */
inline Status queryKernel(NodeExecContext &ctx)
{
  const Scaled s = ctx.queryBlock<Scaled>(ctx.paramBlock("thing").handle);
  return ctx.setOutput(0, GraphValue::ofFloat(s ? s.scale() : -1));
}

/* The value of its thing, of which it uses some aspects. */
inline Status partKernel(NodeExecContext &ctx)
{
  const BlockUse use =
      ctx.paramBlock("thing", ChangeAspects(ctx.param("aspects")->asInt()));
  const Thing *t = static_cast<const Thing *>(use.data);
  return ctx.setOutput(0, GraphValue::ofFloat(t ? t->value : -2));
}

/* A table of `count` elements 0, 1, ...: the keys are the column `i`. */
inline Status rangeKernel(NodeExecContext &ctx)
{
  Elements t;
  Elements::Column c;
  c.name = "i";
  for (int64_t i = 0; i < int64_t(ctx.input(0).asFloat()); i++) {
    t.keys.append(i);
    c.values.append(GraphValue::ofFloat(double(i)));
  }
  t.columns.append(std::move(c));
  return ctx.setOutput(0, tableValue(std::move(t)));
}

/* The sum of a column of a table, `column` by name. */
inline Status totalKernel(NodeExecContext &ctx)
{
  const Elements *t = ctx.input(0).payload<Elements>();
  const Elements::Column *c =
      t ? t->column(ctx.param("column")->asString().c_str()) : nullptr;
  double sum = 0;
  for (int i = 0; c && i < int(c->values.size()); i++) {
    const GraphValue &v = c->values[i];
    sum += v.type() == GraphValue::Type::Float ? v.asFloat() : double(v.asInt());
  }
  return ctx.setOutput(0, GraphValue::ofFloat(sum));
}

inline void addForeachTypes(Runtime &rt)
{
  NodeTypeDef sum("t.sum");
  sum.input(PropertyDef::floatProp("a"))
      .input(PropertyDef::floatProp("b"))
      .output("value", "float");
  addType<sumKernel>(rt, sum, "t.sum");
  NodeTypeDef read("t.read");
  read.param(PropertyDef::pointerProp("thing", kThingStruct)).output("value", "float");
  addType<readKernel>(rt, read, "t.read");
  NodeTypeDef readIf("t.readIf");
  readIf.input(PropertyDef::floatProp("a"))
      .param(PropertyDef::pointerProp("thing", kThingStruct))
      .output("value", "float");
  addType<readIfKernel>(rt, readIf, "t.readIf");
  NodeTypeDef query("t.query");
  query.param(PropertyDef::pointerProp("thing", kThingStruct)).output("value", "float");
  addType<queryKernel>(rt, query, "t.query");
  NodeTypeDef part("t.part");
  part.param(PropertyDef::pointerProp("thing", kThingStruct))
      .param(PropertyDef::intProp("aspects"))
      .output("value", "float");
  addType<partKernel>(rt, part, "t.part");
  NodeTypeDef range("t.range");
  range.input(PropertyDef::floatProp("count")).output("elements", "elements");
  addType<rangeKernel>(rt, range, "t.range");
  NodeTypeDef total("t.total");
  total.payloadInput("elements", "elements")
      .param(PropertyDef::stringProp("column"))
      .output("value", "float");
  addType<totalKernel>(rt, total, "t.total");
}

/* ---- a document ---- */

struct World {
  Runtime rt;
  Library lib;
  BlockId notifyDuring;
  World() : lib(&rt)
  {
    addThings(rt);
    test_assert(registerGraphBlocks(rt).ok() && registerInterface<Scaled>(rt).ok());
    addForeachTypes(rt);
    lib.setQuarantine(true);
  }
  ~World()
  {
    test_assert(rt.validateAll().ok() && lib.validate().ok());
  }
  Thing &thing(BlockHandle h)
  {
    return *static_cast<Thing *>(lib.get(h)->clientData());
  }
  ThingScaled scaled;
  /* Gives a block the interface, with no notification. */
  bool addScaled(BlockHandle h, double value)
  {
    scaled.value = value;
    return registerImpl<Scaled, ThingScaled>(rt, "test.Scaled").ok() &&
           lib.setOverride(h,
                           InterfaceId::of(Scaled::ifaceName),
                           query<Scaled>(rt, TypeId::of("test.Scaled"), &scaled))
               .ok();
  }
  void notify(BlockId id, ChangeAspects aspects = kAllAspects)
  {
    rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(id), {}, aspects);
  }
};

inline PropValue toThing(BlockId id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  r.type = kThingStruct;
  return PropValue::ofPointer(r);
}

/* Writes every block reference of a def, as a Library may. */
inline void redirect(GraphDef &def, Library &lib, BlockId to)
{
  struct Data {
    Library *lib;
    BlockId to;
  } data{&lib, to};
  RefVisitor v(
      [](void *d, RefCell &cell) {
        const Data &r = *static_cast<Data *>(d);
        test_assert(r.lib->writeRef(cell, r.to).ok());
      },
      &data);
  def.foreachRef(v);
}

/* Where an error comes from, whichever element it is in. */
inline NodePath place(const GraphError *e)
{
  NodePath p = e->origin;
  p.elements.clear();
  return p;
}

inline NodePath at(std::initializer_list<NodeId> groups, NodeId node)
{
  NodePath p;
  for (NodeId id : groups) {
    p.groups.append(id);
  }
  p.node = node;
  return p;
}

/*
 * A graph with the input `table`, a foreach node over it, and the output `out`. The
 * foreach's def has the inputs a, b and c, and the outputs sum = a + b (the node `add`)
 * and c2 = c * c (the node `square`).
 */
struct Loop {
  World &w;
  Graph root, inner;
  NodeId in, node, out;
  NodeId innerIn, innerOut, add, square;
  int64_t a = 0, b = 0, c = 0, sum = 0, c2 = 0;
  GraphInstance inst;
  SocketPath path = SocketPath::output("out");
  uint64_t lastRuns = 0;

  explicit Loop(World &world)
      : w(world), root(makeGraph(w.lib, "root")), inner(makeGraph(w.lib, "inner")),
        inst(&w.rt, &w.lib, root.id)
  {
    inst.setUserData(&w.notifyDuring);
    GraphDef &d = *inner.def;
    a = addSocket(d, SocketSide::Input, "a", "float");
    b = addSocket(d, SocketSide::Input, "b", "float");
    c = addSocket(d, SocketSide::Input, "c", "float");
    sum = addSocket(d, SocketSide::Output, "sum", "float");
    c2 = addSocket(d, SocketSide::Output, "c2", "float");
    innerIn = fixture::add(d, "graph.input");
    innerOut = fixture::add(d, "graph.output");
    add = fixture::add(d, "t.sum");
    square = fixture::add(d, "math.float");
    test_assert(d.setParam(square, "op", PropValue::ofEnum(2)).ok());
    connect(d, innerIn, key(a), add, "a");
    connect(d, innerIn, key(b), add, "b");
    connect(d, innerIn, key(c), square, "a");
    connect(d, innerIn, key(c), square, "b");
    connect(d, add, "value", innerOut, key(sum));
    connect(d, square, "value", innerOut, key(c2));

    GraphDef &r = *root.def;
    const int64_t rootTable = addSocket(r, SocketSide::Input, "table", "elements");
    const int64_t rootOut = addSocket(r, SocketSide::Output, "out", "elements");
    in = fixture::add(r, "graph.input");
    out = fixture::add(r, "graph.output");
    node = fixture::add(r, kGraphForeachType);
    test_assert(r.setGroup(node, inner.id).ok());
    connect(r, in, key(rootTable), node, kElementsSocket);
    connect(r, node, kElementsSocket, out, key(rootOut));
  }

  void set(const Elements &t)
  {
    test_assert(inst.setInput("table", tableValue(t)).ok());
  }
  Status evaluate()
  {
    return inst.evaluate(path);
  }
  const Elements *result()
  {
    return inst.value(path) ? inst.value(path)->payload<Elements>() : nullptr;
  }
  /* The first value of a column of the result. */
  const GraphValue *column(const char *name)
  {
    const Elements::Column *col = result() ? result()->column(name) : nullptr;
    return col && col->values.size() > 0 ? &col->values[0] : nullptr;
  }
  const GraphError *error()
  {
    return inst.error(path);
  }
  /* How often the foreach visited its elements since the last call. */
  int ran()
  {
    NodePath p;
    p.node = node;
    const uint64_t now = inst.report(p).runs;
    const int n = int(now - lastRuns);
    lastRuns = now;
    return n;
  }
  /* Evaluates; compares the result, and how many kernels ran. Columns without values
   * aren't compared. */
  bool is(std::initializer_list<int64_t> keys, const Columns &columns, int kernels)
  {
    ran();
    const uint64_t before = inst.totalRuns();
    const Status s = evaluate();
    const int runs = int(inst.totalRuns() - before);
    const Elements *t = s.ok() ? result() : nullptr;
    bool ok = t && runs == kernels && t->size() == int(keys.size()) &&
              std::equal(keys.begin(), keys.end(), t->keys.begin()) &&
              t->columns.size() == columns.size() && t->validate().ok();
    for (int i = 0; ok && i < int(columns.size()); i++) {
      ok = strcmp(t->columns[i].name.c_str(), columns[size_t(i)].name) == 0;
      for (int j = 0; ok && j < int(columns[size_t(i)].values.size()); j++) {
        ok = t->columns[i].values[j] ==
             GraphValue::ofFloat(columns[size_t(i)].values[size_t(j)]);
      }
    }
    if (!ok) {
      printf("%s, %d runs", s.toString().c_str(), runs);
      for (int i = 0; t && i < int(t->columns.size()); i++) {
        printf(", %s:", t->columns[i].name.c_str());
        for (const GraphValue &v : t->columns[i].values) {
          printf(" %g", v.type() == GraphValue::Type::Float ? v.asFloat() : -999.0);
        }
      }
      printf("\n");
    }
    return ok;
  }
  bool
  fails(StatusCode code, std::initializer_list<int64_t> elements, const char *message)
  {
    const Status s = evaluate();
    const GraphError *e = error();
    const bool ok =
        e && s.code() == code && !inst.value(path) &&
        e->origin.elements.size() == elements.size() &&
        std::equal(elements.begin(), elements.end(), e->origin.elements.begin()) &&
        strncmp(e->status.message(), message, strlen(message)) == 0;
    if (!ok) {
      printf("%s\n", s.toString().c_str());
    }
    return ok;
  }
};

} // namespace fixture

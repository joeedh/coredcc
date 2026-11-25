/* Graphs for scripts: defs with all kinds of params, replacing kernels, tables, and what
 * instances tell. */
#include "../graph/edit_fixture.h"
#include "fake_host.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/graph_facades.h"
#include "coredcc/bind/manager.h"

#include <chrono>
#include <cmath>
#include <functional>

using namespace fixture;
namespace sc = coredcc::script;
using fakehost::Host;
using fakehost::Obj;
using litestl::util::string;

test_init;

namespace {

struct Mesh {
  static inline int live = 0;
  Mesh()
  {
    live++;
  }
  Mesh(const Mesh &)
  {
    live++;
  }
  ~Mesh()
  {
    live--;
  }
};

} // namespace

template <> struct coredcc::PayloadTraits<Mesh> {
  static constexpr const char *name = "t.Mesh";
};

namespace {

using Kernel = std::function<Status(NodeExecContext &)>;

Obj kernelObj(Kernel fn)
{
  Obj o;
  o.methods["evaluate"] = [fn](void **args, void *ret, string *) {
    new (ret) Status(fn(*static_cast<NodeExecContext *>(args[0])));
    return true;
  };
  return o;
}

const string kG = "graphs[\"g\"]";

/* Output `value` (float) and input `a` (Float 2). */
NodeTypeDef *newDef(const char *id)
{
  NodeTypeDef *def = sc::newNodeType(id);
  test_assert(sc::nodeTypeInput(*def, "a", sc::makeFloat(2)).ok());
  test_assert(sc::nodeTypeOutput(*def, "value", "float").ok());
  return def;
}

Status finish(Doc &d, Host &host, NodeTypeDef *def, Kernel fn, bool replace = false,
              fakehost::lf::Handle *handle = nullptr, const char *slots = "evaluate")
{
  const fakehost::lf::Handle h = host.add(kernelObj(std::move(fn)));
  if (handle) {
    *handle = h;
  }
  Status s = replace ? sc::replaceForeignNodeType(d.ctx, *def, h, slots) :
                       sc::registerForeignNodeType(d.ctx, *def, h, slots);
  litestl::alloc::Delete(def);
  return s;
}

int64_t addNode(Doc &d, const char *type)
{
  return sc::graphAddNode(d.ctx, kG, type, 0, 0).value;
}

int64_t addSocketTo(Doc &d, bool output, const char *name, const char *type)
{
  return sc::graphAddInterface(d.ctx, kG, output, name, type).value;
}

std::string id(int64_t n)
{
  return std::to_string(n);
}

void testDefs()
{
  Host host;
  {
    Doc d;
    NodeTypeDef *def = newDef("js.full");
    double m[16];
    for (int i = 0; i < 16; i++) {
      m[i] = i;
    }
    sc::ValueResult mat = sc::makeMat4(sc::makeVec4(0, 1, 2, 3), sc::makeVec4(4, 5, 6, 7),
                                       sc::makeVec4(8, 9, 10, 11), sc::makeVec4(12, 13, 14, 15));
    test_assert(mat.value == PropValue::ofComponents(PropValue::Type::Mat4, m));
    test_assert(hasCode(sc::makeMat4(sc::makeVec4(0, 0, 0, 0), sc::makeInt(1), sc::makeInt(1),
                                     sc::makeInt(1))
                            .status,
                        StatusCode::InvalidArgument));
    const PropValue quat = sc::makeQuat(0, 0, 1, 0);
    test_assert(quat == PropValue::ofQuat(0, 0, 1, 0));
    test_assert(sc::nodeTypeInput(*def, "m", mat.value).ok());
    test_assert(sc::nodeTypeParam(*def, "q", quat).ok());
    test_assert(hasCode(sc::nodeTypeParam(*def, "no name", quat), StatusCode::InvalidArgument));
    test_assert(sc::nodeTypeItemParam(*def, "mode", false, 2).ok());
    test_assert(sc::nodeTypeItem(*def, "mode", "A", 1, "Ay").ok());
    test_assert(sc::nodeTypeItem(*def, "mode", "B", 2, "").ok());
    test_assert(sc::nodeTypeItemParam(*def, "flags", true, 4).ok());
    test_assert(sc::nodeTypeItem(*def, "flags", "F", 4, "").ok());
    test_assert(hasCode(sc::nodeTypeItem(*def, "q", "F", 4, ""), StatusCode::TypeMismatch));
    test_assert(hasCode(sc::nodeTypeItem(*def, "no", "F", 4, ""), StatusCode::NotFound));
    test_assert(hasCode(sc::nodeTypeItemParam(*def, "a b", true, 4), StatusCode::InvalidArgument));
    test_assert(sc::nodeTypePointerParam(*def, "thing", "Thing", true).ok());
    test_assert(hasCode(sc::nodeTypePointerParam(*def, "t", "", true), StatusCode::InvalidArgument));
    test_assert(hasCode(sc::nodeTypePointerParam(*def, "", "Thing", true), StatusCode::InvalidArgument));
    test_assert(sc::nodeTypeParam(*def, "n", sc::makeInt(1)).ok());
    test_assert(sc::nodeTypeRange(*def, "n", 0, 5).ok() && sc::nodeTypeRange(*def, "a", 0, 6).ok());
    test_assert(hasCode(sc::nodeTypeRange(*def, "mode", 0, 5), StatusCode::TypeMismatch));
    test_assert(hasCode(sc::nodeTypeRange(*def, "no", 0, 5), StatusCode::NotFound));
    test_assert(sc::nodeTypeSocketName(*def, false, "a", "Amount").ok());
    test_assert(sc::nodeTypeSocketName(*def, true, "value", "Result").ok());
    test_assert(hasCode(sc::nodeTypeSocketName(*def, true, "a", "x"), StatusCode::NotFound));
    test_assert(sc::nodeTypeOutputAdapter(*def).ok());
    string used, linked;
    bool cancelled = true;
    Status noParam, noInput, progress;
    uint32_t aspects = 0;
    int runs = 0;
    auto kernel = [&](NodeExecContext &n) {
      runs++;
      used = sc::nodeUseBlock(n, "thing", aspects).value;
      noParam = sc::nodeUseBlock(n, "a", 0).status;
      noInput = sc::nodeIsLinked(n, "value").status;
      linked = sc::nodeIsLinked(n, "a").value ? "yes" : "no";
      cancelled = sc::nodeCancelled(n);
      progress = sc::nodeProgress(n, 0.5, "half");
      test_assert(sc::nodeParam(n, "mode").value == PropValue::ofEnum(1));
      test_assert(sc::nodeParam(n, "flags").value == PropValue::ofFlags(4));
      test_assert(sc::nodeInput(n, "m").value.component(6) == 6);
      return sc::nodeOutput(n, "value", sc::makeFloat(sc::nodeParam(n, "q").value.component(2)));
    };
    test_assert(finish(d, host, def, kernel).ok());
    const NodeType *t = d.rt.ext<NodeTypeRegistry>().find("js.full");
    test_assert(t->flags == NodeTypeFlags::OutputAdapter && t->params[3].weak);
    test_assert(strcmp(t->inputInfos[0].name.c_str(), "Amount") == 0);
    test_assert(strcmp(t->outputInfos[0].name.c_str(), "Result") == 0);
    test_assert(t->params[1].def.enumMeta()->findKey("A")->value == 1);
    test_assert(t->params[1].def.defaultValue() == PropValue::ofEnum(2));
    test_assert(strcmp(t->params[1].def.enumMeta()->findKey("A")->uiName.c_str(), "Ay") == 0);

    test_assert(sc::graphCreate(d.ctx, "g").status.ok());
    const int64_t n = addNode(d, "js.full"), other = addNode(d, "math.float");
    const std::string node = "graphs[\"g\"].nodes[#" + id(n) + "].";
    test_assert(sc::setValue(d.ctx, (node + "params.mode").c_str(), sc::makeString("A").value).ok());
    test_assert(sc::setValue(d.ctx, (node + "params.n").c_str(), sc::makeInt(9)).ok());
    test_assert(d.is(node + "params.n", PropValue::ofInt(5)));
    const BlockHandle h = newThing(d.lib, "t", 1);
    StableRef ref;
    ref.owner = OwnerKey::ofBlock(idOf(d.lib, h));
    ref.type = kThingStruct;
    test_assert(sc::setValue(d.ctx, (node + "params.thing").c_str(), PropValue::ofPointer(ref)).ok());
    sc::GraphResult g = sc::graphInstance(d.ctx, kG);
    test_assert(g.status.ok());
    const int64_t w = sc::graphWatch(g.value, id(n).c_str(), "value").value;
    auto update = [&]() {
      sc::StringResult r = sc::graphUpdate(g.value);
      test_assert(r.status.ok());
      return std::string(r.value.c_str());
    };
    test_assert(update() == "[" + id(w) + "]");
    test_assert(sc::graphWatchValue(g.value, w).value == sc::makeFloat(1) && runs == 1);
    test_assert(strcmp(used.c_str(), formatBlockId(d.rt, idOf(d.lib, h)).c_str()) == 0);
    test_assert(hasCode(noParam, StatusCode::NotFound) && hasCode(noInput, StatusCode::NotFound));
    test_assert(strcmp(linked.c_str(), "no") == 0 && !cancelled && progress.ok());
    /* Notified of the block, in any aspect or in those it asked for. */
    auto &bus = d.rt.ext<ChangeBus>();
    const OwnerKey thing = ref.owner;
    bus.notify(thing, {}, ChangeAspects(2));
    test_assert(update() == "[]" && runs == 2);
    aspects = 4;
    test_assert(sc::setValue(d.ctx, (node + "params.n").c_str(), sc::makeInt(2)).ok());
    test_assert(sc::graphLink(d.ctx, kG, other, "value", n, "a").status.ok());
    test_assert(update() == "[]" && runs == 3 && strcmp(linked.c_str(), "yes") == 0);
    bus.notify(thing, {}, ChangeAspects(2));
    test_assert(update() == "[]" && runs == 3);
    bus.notify(thing, {}, ChangeAspects(4));
    test_assert(update() == "[]" && runs == 4);
    test_assert(d.lib.markDeleted(h).ok());
    test_assert(update() == "[]" && runs == 5 && used.size() == 0);

    /* Looking at the graph. */
    const int64_t a = sc::graphNewId(d.ctx, kG).value, b = sc::graphNewId(d.ctx, kG).value;
    test_assert(a > 0 && b > 0 && a != b);
    test_assert(hasCode(sc::graphNewId(d.ctx, "graphs").status, StatusCode::InvalidArgument));
    test_assert(strcmp(sc::graphDiagnostics(d.ctx, kG).value.c_str(), "[]") == 0);
    BlockHandle found;
    d.lib.forEach([&](BlockHandle x, const Datablock &k) {
      found = strcmp(k.name().c_str(), "g") == 0 ? x : found;
    });
    GraphDef &gd = *graphDefOf(d.lib, found);
    const Graph inner = newGraph(d.lib, "inner");
    const int64_t x = addSocket(*inner.def, SocketSide::Output, "x", "float");
    const NodeId group = add(gd, kGraphGroupType);
    test_assert(gd.setGroup(group, inner.id).ok());
    const LinkId link = connect(gd, group, key(x), NodeId{other}, "b");
    test_assert(inner.def->removeInterface(x).ok());
    sc::StringResult diag = sc::graphDiagnostics(d.ctx, kG);
    test_assert(diag.status.ok());
    const std::string text = diag.value.c_str();
    test_assert(text.find("\"link\":" + id(link.value)) != std::string::npos);
    test_assert(text.find("\"code\":\"NotFound\"") != std::string::npos);
    test_assert(text.find("\"interior\":false") != std::string::npos);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

void testReplace()
{
  Host host;
  {
    Doc d;
    test_assert(sc::graphCreate(d.ctx, "g").status.ok());
    auto gives = [&](double v) {
      return [v](NodeExecContext &n) { return sc::nodeOutput(n, "value", sc::makeFloat(v)); };
    };
    fakehost::lf::Handle first = 0, second = 0, bad = 0;
    test_assert(finish(d, host, newDef("js.v"), gives(1), false, &first).ok());
    const int64_t n = addNode(d, "js.v");
    sc::GraphResult g = sc::graphInstance(d.ctx, kG);
    const int64_t w = sc::graphWatch(g.value, id(n).c_str(), "value").value;
    auto is = [&](double v) {
      return sc::graphUpdate(g.value).status.ok() &&
             sc::graphWatchValue(g.value, w).value == sc::makeFloat(v);
    };
    test_assert(is(1));
    test_assert(finish(d, host, newDef("js.v"), gives(2), true, &second).ok());
    test_assert(host.releasesOf(first) == 1 && host.releasesOf(second) == 0 && is(2));
    auto refused = [&](const char *name, StatusCode code, const char *slots = "evaluate") {
      const bool ok = hasCode(finish(d, host, newDef(name), gives(3), true, &bad, slots), code);
      return ok && host.releasesOf(bad) == 1 && host.releasesOf(second) == 0;
    };
    test_assert(refused("js.v", StatusCode::TypeMismatch, ""));
    test_assert(refused("js.v", StatusCode::TypeMismatch, "evaluate more"));
    test_assert(refused("js.no", StatusCode::NotFound));
    test_assert(refused("value.float", StatusCode::FailedPrecondition));
    NodeTypeDef *more = newDef("js.v");
    test_assert(sc::nodeTypeParam(*more, "k", sc::makeInt(1)).ok());
    test_assert(hasCode(finish(d, host, more, gives(3), true), StatusCode::FailedPrecondition));
    /* A native one with the same sockets. */
    NodeTypeDef native("t.native");
    native.input(PropertyDef::floatProp("a")).output("value", "float");
    addType<[](NodeExecContext &c) { return c.setOutput(0, GraphValue::ofFloat(0)); }>(
        d.rt, native, "t.native");
    test_assert(refused("t.native", StatusCode::FailedPrecondition));
    NodeTypeDef *def = newDef("js.v");
    test_assert(hasCode(sc::replaceForeignNodeType(d.ctx, *def, 0, "evaluate"),
                        StatusCode::InvalidArgument));
    litestl::alloc::Delete(def);
    test_assert(is(2));
    /* From inside a kernel. */
    Status inside;
    auto replacing = [&](NodeExecContext &c) {
      inside = finish(d, host, newDef("js.v"), gives(4), true, &bad);
      return sc::nodeOutput(c, "value", sc::makeFloat(5));
    };
    test_assert(finish(d, host, newDef("js.v"), replacing, true).ok() && is(5));
    test_assert(hasCode(inside, StatusCode::Busy) && host.releasesOf(bad) == 1);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

sc::ScriptTable makeTable(std::initializer_list<int64_t> keys, const char *name,
                          std::initializer_list<double> values)
{
  sc::ScriptTable t = sc::newTable().value;
  for (int64_t k : keys) {
    test_assert(sc::tableAddRow(t, k).ok());
  }
  test_assert(sc::tableAddColumn(t, name).ok());
  int row = 0;
  for (double v : values) {
    test_assert(sc::tableSet(t, name, row++, sc::makeFloat(v)).ok());
  }
  return t;
}

void testTables()
{
  Host host;
  {
    Doc d;
    sc::ScriptTable t = makeTable({4, 7}, "c", {1, 2});
    test_assert(sc::tableRows(t) == 2 && sc::tableColumns(t) == 1 && t.check().ok());
    test_assert(sc::tableKey(t, 1).value == 7 && sc::tableGet(t, "c", 1).value == sc::makeFloat(2));
    test_assert(strcmp(sc::tableColumnName(t, 0).value.c_str(), "c") == 0);
    for (int32_t i : {-1, 2}) {
      test_assert(hasCode(sc::tableKey(t, i).status, StatusCode::NotFound));
      test_assert(hasCode(sc::tableGet(t, "c", i).status, StatusCode::NotFound));
      test_assert(hasCode(sc::tableSet(t, "c", i, sc::makeInt(1)), StatusCode::NotFound));
    }
    for (int32_t i : {-1, 1}) {
      test_assert(hasCode(sc::tableColumnName(t, i).status, StatusCode::NotFound));
    }
    test_assert(hasCode(sc::tableGet(t, "d", 0).status, StatusCode::NotFound));
    test_assert(hasCode(sc::tableSet(t, "d", 0, sc::makeInt(1)), StatusCode::NotFound));
    test_assert(hasCode(sc::tableSet(t, "c", 0, sc::makeFlags(1)), StatusCode::Unsupported));
    test_assert(hasCode(sc::tableAddColumn(t, "c"), StatusCode::AlreadyExists));
    /* Copies are apart once one is edited. */
    sc::ScriptTable copy = t;
    test_assert(sc::tableAddRow(copy, 9).ok() && sc::tableRows(t) == 2 && sc::tableRows(copy) == 3);
    test_assert(hasCode(copy.check(), StatusCode::InvalidArgument));
    test_assert(sc::tableGet(copy, "c", 2).value.isNone());
    test_assert(sc::tableSet(copy, "c", 2, sc::makeInt(3)).ok() && copy.check().ok());
    test_assert(sc::tableAddColumn(copy, "e").ok() && !copy.check().ok());
    sc::ScriptTable twice = makeTable({4, 4}, "c", {1, 2});
    test_assert(hasCode(twice.check(), StatusCode::InvalidArgument));
    sc::ScriptTable empty;
    test_assert(sc::tableRows(empty) == 0 && sc::tableColumns(empty) == 0 && empty.check().ok());
    test_assert(!sc::tableKey(empty, 0).status.ok() && !sc::tableGet(empty, "c", 0).status.ok());
    test_assert(sc::tableAddRow(empty, 1).ok() && sc::tableRows(empty) == 1);

    /* Through a script kernel: the column doubled, on new keys. */
    NodeTypeDef *def = sc::newNodeType("js.double");
    test_assert(sc::nodeTypePayloadInput(*def, "t", "elements").ok());
    test_assert(sc::nodeTypeInput(*def, "a", sc::makeFloat(2)).ok());
    test_assert(sc::nodeTypeOutput(*def, "t", "elements").ok());
    test_assert(sc::nodeTypeOutput(*def, "value", "float").ok());
    Status notThere, notTable, noOut, notOut, unset;
    Kernel twiceAll = [&](NodeExecContext &n) {
      notThere = sc::nodeInputTable(n, "u").status;
      notTable = sc::nodeInputTable(n, "a").status;
      noOut = sc::nodeOutputTable(n, "u", sc::ScriptTable());
      unset = sc::nodeOutputTable(n, "t", makeTable({1, 2}, "c", {1}));
      sc::TableResult in = sc::nodeInputTable(n, "t");
      test_assert(in.status.ok());
      sc::ScriptTable out = in.value;
      for (int i = 0; i < sc::tableRows(out); i++) {
        test_assert(sc::tableSet(out, "c", i,
                                 sc::makeFloat(sc::tableGet(in.value, "c", i).value.asFloat() * 2))
                        .ok());
      }
      CDCC_TRY(sc::nodeOutput(n, "value", sc::makeFloat(sc::tableRows(in.value))));
      return sc::nodeOutputTable(n, "t", out);
    };
    test_assert(finish(d, host, def, twiceAll).ok());
    test_assert(sc::graphCreate(d.ctx, "g").status.ok());
    const int64_t in = addSocketTo(d, false, "in", "elements");
    const int64_t out = addSocketTo(d, true, "out", "elements");
    const int64_t size = addSocketTo(d, true, "size", "float");
    const int64_t n = addNode(d, "js.double"), i = addNode(d, "graph.input"),
                  o = addNode(d, "graph.output");
    test_assert(sc::graphLink(d.ctx, kG, i, key(in).c_str(), n, "t").status.ok());
    test_assert(sc::graphLink(d.ctx, kG, n, "t", o, key(out).c_str()).status.ok());
    test_assert(sc::graphLink(d.ctx, kG, n, "value", o, key(size).c_str()).status.ok());
    sc::GraphResult g = sc::graphInstance(d.ctx, kG);
    test_assert(sc::graphEvaluate(g.value, "size").value == sc::makeFloat(0));
    test_assert(sc::graphSetInputTable(g.value, "in", t).ok());
    test_assert(sc::graphEvaluate(g.value, "size").value == sc::makeFloat(2));
    sc::TableResult r = sc::graphEvaluateTable(g.value, "out");
    test_assert(r.status.ok() && sc::tableGet(r.value, "c", 1).value == sc::makeFloat(4));
    test_assert(sc::tableGet(t, "c", 1).value == sc::makeFloat(2)); /* not in place */
    test_assert(sc::tableSet(t, "c", 1, sc::makeFloat(50)).ok());
    test_assert(sc::tableGet(sc::graphEvaluateTable(g.value, "out").value, "c", 1).value ==
                sc::makeFloat(4));
    test_assert(hasCode(notThere, StatusCode::NotFound) && hasCode(notTable, StatusCode::TypeMismatch));
    test_assert(hasCode(noOut, StatusCode::NotFound) && hasCode(unset, StatusCode::InvalidArgument));
    test_assert(hasCode(sc::graphEvaluateTable(g.value, "size").status, StatusCode::TypeMismatch));
    test_assert(hasCode(sc::graphEvaluateTable(g.value, "no").status, StatusCode::NotFound));
    test_assert(hasCode(sc::graphEvaluate(g.value, "out").status, StatusCode::Unsupported));
    test_assert(hasCode(sc::graphSetInputTable(g.value, "in", twice), StatusCode::InvalidArgument));
    test_assert(hasCode(sc::graphSetInputTable(g.value, "no", t), StatusCode::NotFound));
    const string node = id(n).c_str();
    test_assert(sc::tableRows(sc::graphPeekTable(g.value, node, "t", false).value) == 2);
    test_assert(sc::tableRows(sc::graphPeekTable(g.value, node, "t", true).value) == 2);
    test_assert(hasCode(sc::graphPeekTable(g.value, node, "value", false).status,
                        StatusCode::TypeMismatch));
    const int64_t w = sc::graphWatch(g.value, "", "out").value;
    test_assert(hasCode(sc::graphWatchTable(g.value, w).status, StatusCode::FailedPrecondition));
    test_assert(sc::graphUpdate(g.value).status.ok());
    test_assert(sc::tableRows(sc::graphWatchTable(g.value, w).value) == 2);
    test_assert(hasCode(sc::graphWatchValue(g.value, w).status, StatusCode::Unsupported));
    /* A payload that is no table. */
    test_assert(d.rt.ext<SocketTypeRegistry>().addPayloadType<Mesh>().ok());
    NodeTypeDef mesh("t.mesh");
    mesh.output("mesh", "t.Mesh");
    addType<[](NodeExecContext &x) {
      return x.setOutput(0, GraphValue::ofPayload(makePayload(Mesh())));
    }>(d.rt, mesh, "t.mesh");
    const int64_t m = addNode(d, "t.mesh");
    const int64_t wm = sc::graphWatch(g.value, id(m).c_str(), "mesh").value;
    test_assert(sc::graphUpdate(g.value).status.ok());
    test_assert(hasCode(sc::graphWatchTable(g.value, wm).status, StatusCode::TypeMismatch));
    /* Unwatched, nothing but the node has its value. */
    test_assert(sc::graphUnwatch(g.value, wm).ok() && Mesh::live == 1);
    test_assert(sc::graphDrop(g.value, id(m).c_str()).ok() && Mesh::live == 0);
    /* A column of payloads can't be read. */
    Elements nested;
    nested.keys.append(1);
    Elements::Column c;
    c.name = "c";
    c.values.append(GraphValue::ofPayload(makePayload(Elements())));
    nested.columns.append(std::move(c));
    sc::ScriptTable outer(makePayload(std::move(nested)));
    test_assert(hasCode(sc::tableGet(outer, "c", 0).status, StatusCode::Unsupported));
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

void testResults()
{
  Host host;
  {
    Doc d;
    test_assert(sc::graphCreate(d.ctx, "g").status.ok());
    Status fail;
    bool asked = false;
    std::function<void()> inside = []() {};
    Kernel kernel = [&](NodeExecContext &n) {
      inside();
      asked = sc::nodeCancelled(n);
      if (asked) {
        return Status::error(StatusCode::Cancelled, "enough");
      }
      CDCC_TRY(Status(fail));
      return sc::nodeOutput(n, "value", sc::nodeInput(n, "a").value);
    };
    test_assert(finish(d, host, newDef("js.f"), kernel).ok());
    const int64_t in = addSocketTo(d, false, "in", "float"), out = addSocketTo(d, true, "out", "float");
    const int64_t i = addNode(d, "graph.input"), o = addNode(d, "graph.output"),
                  n = addNode(d, "js.f"), m = addNode(d, "math.float");
    test_assert(sc::graphLink(d.ctx, kG, i, key(in).c_str(), n, "a").status.ok());
    test_assert(sc::graphLink(d.ctx, kG, n, "value", m, "a").status.ok());
    test_assert(sc::graphLink(d.ctx, kG, m, "value", o, key(out).c_str()).status.ok());
    sc::GraphResult g = sc::graphInstance(d.ctx, kG);
    const string node = id(n).c_str(), sum = id(m).c_str();
    const int64_t wn = sc::graphWatch(g.value, node, "value").value;
    const int64_t wo = sc::graphWatch(g.value, "", "out").value;
    test_assert(wn && wo && wn != wo);
    auto update = [&]() {
      sc::StringResult r = sc::graphUpdate(g.value);
      test_assert(r.status.ok());
      return std::string(r.value.c_str());
    };
    auto report = [&](const string &of) {
      sc::StringResult r = sc::graphReport(g.value, of);
      test_assert(r.status.ok());
      return std::string(r.value.c_str());
    };
    auto has = [&](const string &of, const char *what) {
      return report(of).find(what) != std::string::npos;
    };
    const std::string bothIds = "[" + id(wn) + "," + id(wo) + "]";
    test_assert(sc::graphWatchChangedAt(g.value, wn).value == 0);
    test_assert(hasCode(sc::graphWatchStatus(g.value, wn), StatusCode::FailedPrecondition));
    test_assert(sc::graphWatchLastGood(g.value, wn).value.isNone());
    test_assert(sc::graphPeek(g.value, node, "value", false).value.isNone());
    test_assert(has(node, "\"state\":\"NotEvaluated\""));
    test_assert(sc::graphSetInput(g.value, "in", sc::makeFloat(3)).ok() && update() == bothIds);
    test_assert(update() == "[]" && sc::graphWatchStatus(g.value, wn).ok());
    test_assert(sc::graphWatchValue(g.value, wn).value == sc::makeFloat(3));
    test_assert(sc::graphWatchIsCurrent(g.value, wn).value);
    const int64_t at = sc::graphWatchChangedAt(g.value, wn).value;
    test_assert(at > 0 && sc::graphPeek(g.value, node, "a", true).value == sc::makeFloat(3));
    test_assert(sc::graphPeek(g.value, sum, "value", false).value == sc::makeFloat(3));
    test_assert(sc::graphPeek(g.value, "", "out", false).value == sc::makeFloat(3));
    test_assert(has(node, "\"state\":\"Ok\",\"current\":true,\"runs\":1") && !has(node, "error"));
    test_assert(has(node, "\"kernels\":1"));

    /* Errors. */
    fail = Status::error(StatusCode::OutOfRange, "too far");
    test_assert(sc::graphSetInput(g.value, "in", sc::makeFloat(4)).ok());
    test_assert(!sc::graphWatchIsCurrent(g.value, wn).value && update() == bothIds);
    test_assert(hasCode(sc::graphWatchStatus(g.value, wo), StatusCode::OutOfRange));
    test_assert(hasCode(sc::graphWatchValue(g.value, wo).status, StatusCode::OutOfRange));
    test_assert(hasCode(sc::graphWatchTable(g.value, wo).status, StatusCode::OutOfRange));
    test_assert(sc::graphWatchLastGood(g.value, wo).value == sc::makeFloat(3));
    test_assert(sc::graphWatchChangedAt(g.value, wn).value > at);
    test_assert(strstr(sc::graphWatchErrorText(g.value, wo).c_str(), ("(node " + id(n) + ")").c_str()));
    test_assert(has(node, ("\"error\":{\"code\":\"OutOfRange\",\"message\":\"too far\",\"node\":\"" +
                           id(n) + "\"}")
                              .c_str()));
    test_assert(has(node, "\"state\":\"Failed\"") && has(sum, "\"state\":\"InputFailed\""));
    fail = Status();
    test_assert(update() == "[]" && sc::graphRetry(g.value).ok() && update() == bothIds);
    test_assert(sc::graphWatchErrorText(g.value, wo).size() == 0);

    /* Dropped, and cleared. */
    test_assert(sc::graphDrop(g.value, node).ok() && has(node, "\"state\":\"NotEvaluated\""));
    test_assert(hasCode(sc::graphDrop(g.value, "x"), StatusCode::InvalidArgument));
    test_assert(update() == "[]" && has(node, "\"state\":\"Ok\""));
    test_assert(sc::graphClear(g.value).ok() && has(node, "\"state\":\"NotEvaluated\""));
    test_assert(update() == "[]" || sc::graphWatchStatus(g.value, wn).ok());

    /* A time limit. */
    test_assert(sc::graphSetTimeLimit(g.value, 1e-9).ok());
    test_assert(sc::graphSetInput(g.value, "in", sc::makeFloat(5)).ok());
    test_assert(update() == bothIds);
    test_assert(hasCode(sc::graphWatchStatus(g.value, wn), StatusCode::Cancelled) && !asked);
    test_assert(hasCode(sc::graphEvaluate(g.value, "out").status, StatusCode::Cancelled));
    test_assert(sc::graphSetTimeLimit(g.value, 1e9).ok() && update() == bothIds);
    test_assert(sc::graphWatchValue(g.value, wo).value == sc::makeFloat(5));
    test_assert(sc::graphSetTimeLimit(g.value, 0).ok());
    test_assert(sc::graphSetInput(g.value, "in", sc::makeFloat(6)).ok() && update() == bothIds);
    for (double bad : {-1.0, double(NAN)}) {
      test_assert(hasCode(sc::graphSetTimeLimit(g.value, bad), StatusCode::InvalidArgument));
    }
    /* The limit counts from the call's start: in a kernel that takes longer, it is told. */
    test_assert(sc::graphSetTimeLimit(g.value, 50).ok());
    inside = []() {
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
      while (std::chrono::steady_clock::now() < until) {
      }
    };
    test_assert(sc::graphSetInput(g.value, "in", sc::makeFloat(7)).ok() && update() == bothIds);
    test_assert(asked && strstr(sc::graphWatchErrorText(g.value, wo).c_str(), "enough"));
    inside = []() {};
    test_assert(update() == bothIds && sc::graphSetTimeLimit(g.value, 0).ok());

    /* Bad arguments. */
    test_assert(hasCode(sc::graphWatch(g.value, "x", "value").status, StatusCode::InvalidArgument));
    test_assert(hasCode(sc::graphPeek(g.value, "1/", "value", false).status, StatusCode::InvalidArgument));
    test_assert(hasCode(sc::graphPeekTable(g.value, "1/", "a", false).status, StatusCode::InvalidArgument));
    test_assert(hasCode(sc::graphReport(g.value, "").status, StatusCode::InvalidArgument));
    const int64_t gone = sc::graphWatch(g.value, node, "no").value;
    test_assert(gone && sc::graphUpdate(g.value).status.ok());
    test_assert(hasCode(sc::graphWatchStatus(g.value, gone), StatusCode::NotFound));
    test_assert(sc::graphUnwatch(g.value, gone).ok() && update() == "[]");
    std::vector<Status> none = {sc::graphUnwatch(g.value, gone),
                                sc::graphWatchStatus(g.value, gone),
                                sc::graphWatchValue(g.value, gone).status,
                                sc::graphWatchTable(g.value, gone).status,
                                sc::graphWatchLastGood(g.value, gone).status,
                                sc::graphWatchChangedAt(g.value, gone).status,
                                sc::graphWatchIsCurrent(g.value, gone).status};
    for (const Status &s : none) {
      test_assert(hasCode(s, StatusCode::NotFound));
    }
    test_assert(strstr(sc::graphWatchErrorText(g.value, gone).c_str(), "no watch"));

    /* From a kernel, and once it is closed. */
    std::vector<Status> seen;
    auto all = [&]() {
      seen = {sc::graphWatch(g.value, node, "value").status,
              sc::graphUnwatch(g.value, wn),
              sc::graphUpdate(g.value).status,
              sc::graphEvaluateTable(g.value, "out").status,
              sc::graphSetInputTable(g.value, "in", sc::ScriptTable()),
              sc::graphWatchStatus(g.value, wn),
              sc::graphWatchValue(g.value, wn).status,
              sc::graphWatchTable(g.value, wn).status,
              sc::graphWatchLastGood(g.value, wn).status,
              sc::graphWatchChangedAt(g.value, wn).status,
              sc::graphWatchIsCurrent(g.value, wn).status,
              sc::graphPeek(g.value, node, "value", false).status,
              sc::graphPeekTable(g.value, node, "value", false).status,
              sc::graphReport(g.value, node).status,
              sc::graphRetry(g.value),
              sc::graphClear(g.value),
              sc::graphDrop(g.value, node),
              sc::graphSetTimeLimit(g.value, 5)};
    };
    inside = all;
    Status ids, diagnostics;
    inside = [&]() {
      all();
      ids = sc::graphNewId(d.ctx, kG).status;
      diagnostics = sc::graphDiagnostics(d.ctx, kG).status;
    };
    test_assert(sc::graphSetInput(g.value, "in", sc::makeFloat(8)).ok() && update() == bothIds);
    test_assert(seen.size() == 18 && hasCode(ids, StatusCode::Busy));
    test_assert(hasCode(diagnostics, StatusCode::Busy));
    for (size_t k = 0; k < seen.size(); k++) {
      const bool busy = hasCode(seen[k], StatusCode::Busy);
      test_assert(busy || !printf("call %d\n", int(k)));
    }
    inside = []() {};
    sc::graphClose(g.value);
    all();
    for (const Status &s : seen) {
      test_assert(hasCode(s, StatusCode::FailedPrecondition));
    }
    test_assert(strstr(sc::graphWatchErrorText(g.value, wn).c_str(), "closed"));
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

/* A kernel that drops the last handle of the graph that is evaluating, with a time limit. */
void testDroppedInside()
{
  Host host;
  {
    Doc d;
    test_assert(sc::graphCreate(d.ctx, "g").status.ok());
    sc::GraphResult g;
    Kernel kernel = [&](NodeExecContext &n) {
      g = sc::GraphResult();
      test_assert(!sc::nodeCancelled(n));
      return sc::nodeOutput(n, "value", sc::makeFloat(1));
    };
    test_assert(finish(d, host, newDef("js.f"), kernel).ok());
    const int64_t n = addNode(d, "js.f"), m = addNode(d, "math.float");
    test_assert(sc::graphLink(d.ctx, kG, n, "value", m, "a").status.ok());
    g = sc::graphInstance(d.ctx, kG);
    test_assert(sc::graphSetTimeLimit(g.value, 1e9).ok());
    test_assert(sc::graphWatch(g.value, id(m).c_str(), "value").status.ok());
    test_assert(sc::graphUpdate(g.value).status.ok());
    test_assert(sc::graphInstanceCount(d.ctx) == 0);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

} // namespace

int main()
{
  testDefs();
  testReplace();
  testTables();
  testResults();
  testDroppedInside();
  return test_end();
}

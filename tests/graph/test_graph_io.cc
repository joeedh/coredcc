/* Graphs in files: saving and loading, appending, duplicating, and types that aren't there. */
#include "edit_fixture.h"
#include "foreach_fixture.h"
#include "io_compare.h"

#include "coredcc/api/library_io.h"
#include "coredcc/graph/instance.h"
#include "coredcc/lib/duplicate.h"

using namespace fixture;

test_init;

namespace {

/* ---- types ---- */

void addSum(Runtime &rt)
{
  NodeTypeDef t("t.sum");
  t.input(PropertyDef::floatProp("a")).input(PropertyDef::floatProp("b")).output("value", "float");
  addType<sumKernel>(rt, t, "t.sum");
}

Status modeKernel(NodeExecContext &ctx)
{
  const int64_t mode = ctx.param("mode")->asEnum();
  const double a = ctx.input("a")->asFloat();
  return ctx.setOutput(0, GraphValue::ofFloat(mode % 10 == 1 ? a : -a));
}

/* Enum items' values depend on the build; here they all are 1 and 2 modulo 10. */
void addMode(Runtime &rt, int64_t base)
{
  NodeTypeDef t("io.mode");
  t.input(PropertyDef::floatProp("a"))
      .param(PropertyDef::enumProp("mode").item("KEEP", base + 1).item("NEGATE", base + 2))
      .param(PropertyDef::flagsProp("flags").item("A", base ? 4 : 1).item("B", 2))
      .param(PropertyDef::stringProp("name"))
      .param(PropertyDef::pointerProp("strong", StructId::of(kGraphDefType)))
      .param(PropertyDef::pointerProp("weak", StructId::of(kGraphDefType)), true)
      .output("value", "float");
  addType<modeKernel>(rt, t, "io.mode");
}

PropValue graphPointer(const Graph &g)
{
  return pointerTo(g);
}

Graph byName(Library &lib, const char *name)
{
  Graph g;
  lib.forEach([&](BlockHandle h, const Datablock &b) {
    if (strcmp(b.name().c_str(), name) == 0 && b.state() == BlockState::Live) {
      g = Graph{h, b.id(), graphDefOf(lib, h)};
    }
  });
  return g;
}

/* Two graphs with one of everything; "root" uses "inner". */
struct Content {
  Graph root, inner;
  NodeId in, out, group, each, mode, sum, late;
  int64_t i = 0, o = 0, a = 0, innerOut = 0;

  explicit Content(Library &lib) : root(newGraph(lib, "root")), inner(newGraph(lib, "inner"))
  {
    lib.setFlag(root.h, BlockFlags::Root, true);
    GraphDef &d = *inner.def;
    a = addSocket(d, SocketSide::Input, "a", "float");
    innerOut = addSocket(d, SocketSide::Output, "o", "float");
    test_assert(d.setInterfaceDefault(a, PropValue::ofFloat(2)).ok());
    const NodeId din = add(d, "graph.input"), dout = add(d, "graph.output"), s = add(d, "t.sum");
    connect(d, din, key(a), s, "a");
    connect(d, s, "value", dout, key(innerOut));
    test_assert(d.setLiteral(s, "b", PropValue::ofFloat(0.25)).ok());

    GraphDef &r = *root.def;
    o = addSocket(r, SocketSide::Output, "out", "float");
    i = addSocket(r, SocketSide::Input, "in", "int");
    addSocket(r, SocketSide::Input, "unset", "mat4");
    test_assert(r.setInterfaceDefault(i, PropValue::ofInt(7)).ok());
    in = add(r, "graph.input");
    out = add(r, "graph.output");
    group = add(r, "graph.group");
    each = add(r, kGraphForeachType);
    mode = add(r, "io.mode");
    sum = add(r, "t.sum");
    test_assert(r.setGroup(group, inner.id).ok() && r.setGroup(each, inner.id).ok());
    test_assert(r.setParam(each, kMaxCachedParam, PropValue::ofInt(3)).ok());
    test_assert(r.setLabel(mode, "a label").ok() && r.setPosition(mode, -1.5, 1e300).ok());
    test_assert(r.setParam(mode, "mode", PropValue::ofString("NEGATE")).ok());
    test_assert(r.setParam(mode, "flags", PropValue::ofString("A|B")).ok());
    test_assert(r.setParam(mode, "name", PropValue::ofString("text")).ok());
    test_assert(r.setParam(mode, "strong", graphPointer(inner)).ok());
    test_assert(r.setParam(mode, "weak", graphPointer(inner)).ok());
    connect(r, in, key(i), group, key(a));
    connect(r, group, key(innerOut), mode, "a");
    connect(r, mode, "value", sum, "a");
    connect(r, sum, "value", out, key(o)); /* -7.25 + b */
    /* A literal out of use, and one of each type of a socket that changed its type. */
    test_assert(r.setLiteral(sum, "b", PropValue::ofFloat(100)).ok());
    test_assert(r.useLiteral(sum, "b", false).ok());
    test_assert(r.setLiteral(each, key(a), PropValue::ofFloat(3)).ok());
    test_assert(d.setInterfaceType(a, "int").ok());
    test_assert(r.setLiteral(each, key(a), PropValue::ofInt(4)).ok());
    test_assert(d.setInterfaceType(a, "float").ok());
    test_assert(r.node(each)->literals().size() == 2);
    test_assert(r.setExtra(mode, "ui.width", PropValue::ofFloat(80)).ok());
    test_assert(r.setExtra(mode, "ui.color", PropValue::ofVec3(1, 0, 0)).ok());
    /* A node whose type isn't there, with links, and a broken link. */
    NodeState s2;
    s2.id = NodeId{41};
    s2.typeName = "late.sum";
    s2.typeVersion = 7;
    /* Extras of another build: of a type this one doesn't know, and with a reference. */
    BlockEntry unknown, pointer;
    unknown.name = "new.kind";
    unknown.unknownTag = 200;
    unknown.raw.append(3);
    pointer.name = "new.graph";
    pointer.value = graphPointer(inner);
    pointer.refs.append(RefCell(RefKind::Weak));
    pointer.refs[0].target = inner.id;
    s2.extras.append(unknown);
    s2.extras.append(pointer);
    s2.params.append(NodeValue{"p", PropValue::ofVec3(1, 2, 3)});
    s2.literals.append(NodeValue{"a", PropValue::ofFloat(6)});
    s2.refs.append(NodeState::Ref{"gone", inner.id, true});
    s2.inputs.append(SocketInfo{"a", "The A", "float"});
    s2.outputs.append(SocketInfo{"value", "value", "late.type"});
    test_assert(r.insertNode(s2).ok());
    late = s2.id;
    const LinkState l{LinkId{42}, late, "value", sum, "c"}, m{LinkId{43}, sum, "value", late, "a"};
    test_assert(r.insertLink(l).ok() && r.insertLink(m).ok());
    connect(r, in, key(i), group, "@" + std::to_string(s.value) + ".b");
  }
};

constexpr double kResult = -14; /* of `out`: in + in, negated */

bool evaluates(Runtime &rt, Library &lib, double expected, const char *graph = "root")
{
  GraphInstance inst(&rt, &lib, byName(lib, graph).id);
  const SocketPath p = SocketPath::output("out");
  const Status s = inst.evaluate(p);
  if (!s.ok() || inst.value(p)->asFloat() != expected) {
    printf("%s, %g\n", s.toString().c_str(), s.ok() ? inst.value(p)->asFloat() : 0);
  }
  return s.ok() && inst.value(p)->asFloat() == expected;
}

using Bytes = litestl::util::Vector<uint8_t>;
std::span<const uint8_t> view(const Bytes &b)
{
  return std::span<const uint8_t>(b.data(), size_t(b.size()));
}
Bytes save(const Library &lib)
{
  Result<Bytes> b = saveLibrary(lib);
  if (!b.ok()) {
    printf("save: %s\n", b.status().toString().c_str());
  }
  test_assert(b.ok());
  return *b;
}
LoadReport load(Context &ctx, const Bytes &bytes)
{
  Result<LoadReport> r = loadLibrary(ctx, view(bytes));
  test_assert(r.ok());
  for (const IoIssue &i : r->issues.first) {
    printf("issue: %s\n", i.why.toString().c_str());
  }
  return *r;
}
bool same(const Bytes &a, const Bytes &b)
{
  return a.size() == b.size() && memcmp(a.data(), b.data(), size_t(a.size())) == 0;
}

struct IoDoc : Doc {
  explicit IoDoc(int64_t base = 0)
  {
    addSum(rt);
    addMode(rt, base);
  }
};

/* ---- saving and loading ---- */

void testRoundTrip()
{
  IoDoc a;
  Content c(a.lib);
  test_assert(evaluates(a.rt, a.lib, kResult));
  SaveReport report;
  test_assert(saveLibrary(a.lib, &report).ok() && report.blocks == 2 && report.issues.total == 0);
  const Bytes bytes = save(a.lib);
  test_assert(same(bytes, save(a.lib)));

  IoDoc b;
  const LoadReport loaded = load(b.ctx, bytes);
  test_assert(loaded.issues.total == 0 && loaded.records == 2 && loaded.missingType == 0);
  const Graph root = byName(b.lib, "root"), inner = byName(b.lib, "inner");
  test_assert(root.id == c.root.id && root.def->self() == root.id && inner.def->self() == inner.id);
  test_assert(sameGraph(*c.root.def, *root.def) && sameGraph(*c.inner.def, *inner.def));
  test_assert(root.def->validate().ok() && root.def->library() == &b.lib);
  test_assert(evaluates(b.rt, b.lib, kResult) && same(bytes, save(b.lib)));
  test_assert(root.def->serial() != inner.def->serial());

  /* Strengths: the group and `strong` keep inner, `weak` doesn't. */
  test_assert(b.lib.collect().ok() && !b.lib.find(inner.id).isNull());
  test_assert(root.def->setGroup(c.group, BlockId()).ok() && root.def->setGroup(c.each, BlockId()).ok());
  test_assert(b.lib.collect().ok() && !b.lib.find(inner.id).isNull());
  test_assert(root.def->setParam(c.mode, "strong", PropValue()).ok() && b.lib.collect().ok());
  test_assert(b.lib.find(inner.id).isNull());
}

/* What is loaded is edited and evaluated like anything else. */
void testEdits()
{
  IoDoc a, b;
  Content c(a.lib);
  load(b.ctx, save(a.lib));
  const Graph root = byName(b.lib, "root"), inner = byName(b.lib, "inner");
  GraphInstance inst(&b.rt, &b.lib, root.id);
  const SocketPath p = SocketPath::output("out");
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == kResult);
  test_assert(root.def->useLiteral(c.sum, "b", true).ok());
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == kResult + 100);
  test_assert(runGraphEdit(b.ctx, root.id, "ungroup", [&](GraphEdit &e) {
                return e.setGroup(c.group, BlockId());
              }).ok());
  test_assert(!inst.evaluate(p).ok() && b.undo().ok());
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == kResult + 100);
  test_assert(hasCode(root.def->setGroup(c.group, root.id), StatusCode::InvalidArgument));
  test_assert(hasCode(root.def->link({c.sum, "value"}, {c.mode, "a"}), StatusCode::InvalidArgument));
  test_assert(hasCode(root.def->addNode("graph.output"), StatusCode::AlreadyExists));
  /* New ids are new, and an edit's version is after everything that was read. */
  const uint64_t version = root.def->node(c.mode)->paramVersion();
  test_assert(root.def->setParam(c.mode, "mode", PropValue::ofString("KEEP")).ok());
  test_assert(root.def->node(c.mode)->paramVersion() > version);
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == 114);
  test_assert(inner.def->setInterfaceDefault(c.a, PropValue::ofFloat(9)).ok());
}

/* A build without the types reads and writes it without loss, and has them later. */
void testMissingTypes()
{
  IoDoc a;
  Content c(a.lib);
  const Bytes bytes = save(a.lib);
  Doc b;
  test_assert(load(b.ctx, bytes).issues.total == 0);
  const Graph root = byName(b.lib, "root");
  test_assert(!root.def->node(c.mode)->type() && root.def->node(c.group)->type());
  test_assert(root.def->sockets(*root.def->node(c.mode), SocketSide::Input).size() == 1);
  test_assert(root.def->diagnostics().size() == 1); /* a socket type isn't there either */
  GraphInstance inst(&b.rt, &b.lib, root.id);
  const SocketPath p = SocketPath::output("out");
  test_assert(hasCode(inst.evaluate(p), StatusCode::NotFound));
  const Bytes again = save(b.lib);
  IoDoc d;
  load(d.ctx, again);
  test_assert(sameGraph(*c.root.def, *byName(d.lib, "root").def));
  test_assert(evaluates(d.rt, d.lib, kResult) && same(save(d.lib), bytes) && same(again, bytes));
  /* A type with a pointer that the file doesn't have, and without one that it has. */
  IoDoc e;
  NodeTypeDef t("late.sum");
  t.param(PropertyDef::pointerProp("extra", StructId::of(kGraphDefType)), true);
  t.output("value", "float");
  addType<modeKernel>(e.rt, t, "late.sum");
  load(e.ctx, bytes);
  const Graph read = byName(e.lib, "root");
  const GraphNode &late = *read.def->node(c.late);
  test_assert(late.blockRefs().size() == 2 && late.blockTarget("gone") == c.inner.id);
  test_assert(strcmp(late.blockRefs()[1].name.c_str(), "extra") == 0 && late.blockRefs()[1].cell.kind == RefKind::Weak);
  test_assert(!read.def->setParam(c.late, "extra", graphPointer(read)).ok()); /* itself */
  test_assert(read.def->setParam(c.late, "extra", graphPointer(byName(e.lib, "inner"))).ok());
  save(e.lib);
  /* Later. */
  addSum(b.rt);
  addMode(b.rt, 0);
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == kResult);
  test_assert(same(save(b.lib), bytes));
}

/* Whether a reference is strong is the type's to say, not the file's or a state's. */
Status noneKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofFloat(0));
}
void addKeeper(Runtime &rt, bool weak)
{
  NodeTypeDef t("io.keeper");
  t.param(PropertyDef::pointerProp("keeps", StructId::of(kGraphDefType)), weak);
  t.param(PropertyDef::pointerProp("other", StructId::of(kGraphDefType)), !weak);
  t.output("value", "float");
  addType<noneKernel>(rt, t, "io.keeper");
}

void testStrength()
{
  for (const bool weak : {false, true}) {
    /* A state says the opposite of the type: known at once, or later. */
    for (int late = 0; late < 3; late++) {
      IoDoc a;
      const Graph root = newGraph(a.lib, "root"), target = newGraph(a.lib, "target");
      a.lib.setFlag(root.h, BlockFlags::Root, true);
      if (late == 0) {
        addKeeper(a.rt, weak);
      }
      NodeState s;
      s.id = NodeId{5};
      s.typeName = "io.keeper";
      s.refs.append(NodeState::Ref{"keeps", target.id, !weak});
      test_assert(root.def->insertNode(s).ok());
      Graph at = root;
      IoDoc b;
      Doc *d = &a;
      if (late == 2) { /* from a file, whose block is made before the type is */
        a.lib.setFlag(target.h, BlockFlags::Root, true); /* so that it is saved */
        load(b.ctx, save(a.lib));
        at = byName(b.lib, "root");
        b.lib.setFlag(byName(b.lib, "target").h, BlockFlags::Root, false);
        d = &b;
      }
      if (late != 0) {
        /* The state's word counts while there is none of the type's. */
        test_assert(at.def->node(s.id)->blockRefs()[0].cell.kind ==
                    (weak ? RefKind::OptStrong : RefKind::Weak));
        test_assert(d->lib.usersOf(byName(d->lib, "target").h).size() == 1);
        addKeeper(d->rt, weak);
      }
      const auto &refs = at.def->node(s.id)->blockRefs();
      test_assert(refs.size() == 2 && at.def->validate().ok());
      test_assert(refs[0].cell.kind == (weak ? RefKind::Weak : RefKind::OptStrong));
      test_assert(refs[1].cell.kind == (weak ? RefKind::OptStrong : RefKind::Weak));
      test_assert(d->lib.collect().ok()); /* which checks the users it knows, too */
      test_assert(!byName(d->lib, "target").h.isNull() == !weak);
    }
    /* A file says the opposite. */
    IoDoc a, b;
    const Graph root = newGraph(a.lib, "root"), target = newGraph(a.lib, "target");
    a.lib.setFlag(root.h, BlockFlags::Root, true);
    addKeeper(a.rt, !weak);
    addKeeper(b.rt, weak);
    const NodeId n = add(*root.def, "io.keeper");
    test_assert(root.def->setParam(n, "keeps", graphPointer(target)).ok());
    a.lib.setFlag(target.h, BlockFlags::Root, true);
    load(b.ctx, save(a.lib));
    b.lib.setFlag(byName(b.lib, "target").h, BlockFlags::Root, false);
    const Graph read = byName(b.lib, "root");
    test_assert(read.def->node(n)->blockTarget("keeps") == target.id);
    test_assert(read.def->node(n)->blockRefs()[0].cell.kind ==
                (weak ? RefKind::Weak : RefKind::OptStrong));
    test_assert(b.lib.collect().ok() && !byName(b.lib, "target").h.isNull() == !weak);
    /* Undo of removing a node. */
    test_assert(runGraphEdit(b.ctx, read.id, "remove", [&](GraphEdit &e) {
                  return e.removeNode(n);
                }).ok() && b.undo().ok());
    test_assert(read.def->node(n)->blockRefs()[0].cell.kind ==
                (weak ? RefKind::Weak : RefKind::OptStrong));
  }
}

/* Node types have versions, which nodes keep. */
void testVersions()
{
  auto addVersion = [](Runtime &rt, uint32_t version) {
    NodeTypeDef t("io.versioned");
    t.output("value", "float").version(version);
    NodeTypeDef bad = t.copy();
    test_assert(hasCode(rt.ext<NodeTypeRegistry>().check(bad.version(0)),
                        StatusCode::InvalidArgument));
    t = t.copy();
    addType<noneKernel>(rt, t, "io.versioned");
    test_assert(rt.ext<NodeTypeRegistry>().find("io.versioned")->version == version);
  };
  IoDoc a, b, c;
  addVersion(a.rt, 2);
  addVersion(b.rt, 3);
  const Graph g = newGraph(a.lib, "root");
  a.lib.setFlag(g.h, BlockFlags::Root, true);
  const NodeId n = add(*g.def, "io.versioned"), sum = add(*g.def, "t.sum");
  test_assert(g.def->node(n)->typeVersion() == 2 && g.def->node(sum)->typeVersion() == 1);
  test_assert(g.def->stateOf(*g.def->node(n)).typeVersion == 2);
  const Bytes bytes = save(a.lib);
  load(b.ctx, bytes);
  load(c.ctx, bytes); /* without the type */
  const Graph newer = byName(b.lib, "root"), none = byName(c.lib, "root");
  test_assert(newer.def->node(n)->typeVersion() == 2 && none.def->node(n)->typeVersion() == 2);
  addVersion(c.rt, 1);
  test_assert(none.def->node(n)->typeVersion() == 2 && same(save(b.lib), bytes) &&
              same(save(c.lib), bytes));
  /* A new node has the type's; a state may say another, or nothing. */
  test_assert(newer.def->node(add(*newer.def, "io.versioned"))->typeVersion() == 3);
  NodeState s;
  s.id = NodeId{1};
  s.typeName = "io.versioned";
  test_assert(newer.def->insertNode(s).ok() && newer.def->node(s.id)->typeVersion() == 3);
  s.id = NodeId{2};
  s.typeVersion = 9;
  test_assert(newer.def->insertNode(s).ok() && newer.def->node(s.id)->typeVersion() == 9);
  s.id = NodeId{3};
  s.typeVersion = 0;
  s.typeName = "io.unknown";
  test_assert(newer.def->insertNode(s).ok() && newer.def->node(s.id)->typeVersion() == 1);
}

/* An extra with a reference, which only a newer build makes. */
void testExtraRefs()
{
  IoDoc d;
  Content c(d.lib);
  const Graph kept = newGraph(d.lib, "kept"), alone = newGraph(d.lib, "alone");
  NodeState s;
  s.id = NodeId{77};
  s.typeName = "t.sum";
  BlockEntry e;
  e.name = "new.graph";
  e.value = graphPointer(kept);
  e.refs.append(RefCell(RefKind::OptStrong));
  e.refs[0].target = kept.id;
  s.extras.append(e);
  GraphDef standalone(&d.rt);
  test_assert(hasCode(standalone.insertNode(s), StatusCode::FailedPrecondition));
  test_assert(d.lib.usersOf(kept.h).size() == 0);
  test_assert(c.root.def->insertNode(s).ok() && d.lib.usersOf(kept.h).size() == 1);
  test_assert(d.lib.collect().ok() && d.lib.get(kept.h) && !d.lib.get(alone.h));
  /* Undo brings it back, and holds the block until then. */
  auto edit = [&](auto fn) { return runGraphEdit(d.ctx, c.root.id, "edit", fn).ok(); };
  test_assert(edit([&](GraphEdit &g) { return g.removeNode(s.id); }));
  test_assert(d.lib.usersOf(kept.h).size() == 0 && d.lib.collect().ok() && d.lib.get(kept.h));
  test_assert(d.undo().ok() && d.lib.usersOf(kept.h).size() == 1);
  test_assert(edit([&](GraphEdit &g) { return g.setExtra(s.id, "new.graph", PropValue::ofInt(1)); }));
  test_assert(d.lib.usersOf(kept.h).size() == 0 && d.lib.collect().ok() && d.lib.get(kept.h));
  test_assert(d.undo().ok() && d.lib.usersOf(kept.h).size() == 1);
  IoDoc b;
  load(b.ctx, save(d.lib));
  test_assert(sameGraph(*c.root.def, *byName(b.lib, "root").def) && byName(b.lib, "kept").def);
  test_assert(c.root.def->removeNode(s.id).ok() && d.lib.usersOf(kept.h).size() == 0);
  d.history.clear();
  test_assert(d.lib.collect().ok() && !d.lib.get(kept.h));
}

/* Enums are keys in a file. */
void testEnums()
{
  IoDoc a, b(20);
  Content c(a.lib);
  load(b.ctx, save(a.lib));
  const Graph root = byName(b.lib, "root");
  const GraphNode &n = *root.def->node(c.mode);
  test_assert(root.def->paramValue(n, "mode") == PropValue::ofEnum(22));
  test_assert(root.def->paramValue(n, "flags") == PropValue::ofFlags(6));
  test_assert(evaluates(b.rt, b.lib, kResult) && same(save(a.lib), save(b.lib)));
}

/* Targets that aren't saved. */
void testTargets()
{
  IoDoc a, b;
  Content c(a.lib);
  const Graph lost = newGraph(a.lib, "lost");
  GraphDef &r = *c.root.def;
  const NodeId m = add(r, "io.mode"), g = add(r, "graph.group");
  test_assert(r.setParam(m, "strong", graphPointer(lost)).ok());
  test_assert(r.setParam(m, "weak", graphPointer(lost)).ok() && r.setGroup(g, lost.id).ok());
  test_assert(a.lib.markDeleted(lost.h).ok());
  load(b.ctx, save(a.lib));
  const Graph root = byName(b.lib, "root");
  test_assert(root.def->node(g)->group().isNull());
  test_assert(root.def->node(m)->blockTarget("strong").isNull() && evaluates(b.rt, b.lib, kResult));
  /* Only the cell of a group node counts, also when the Library wrote it. */
  test_assert(a.lib.restoreFromHistory(lost.h).ok());
  a.lib.writeRef(const_cast<RefCell &>(r.node(g)->group()), c.inner.id).ignore();
  a.lib.writeRef(const_cast<RefCell &>(r.node(m)->blockRefs()[0].cell), c.inner.id).ignore();
  IoDoc d;
  load(d.ctx, save(a.lib));
  const Graph again = byName(d.lib, "root");
  test_assert(again.def->node(g)->group().target == c.inner.id);
  test_assert(again.def->node(m)->blockTarget("strong") == c.inner.id);
}

/* Graphs that reach themselves, or have a cycle, load; the evaluator says what is wrong. */
void testCycles()
{
  IoDoc a, b;
  const Graph g = newGraph(a.lib, "root"), h = newGraph(a.lib, "other");
  a.lib.setFlag(g.h, BlockFlags::Root, true);
  const int64_t o = addSocket(*g.def, SocketSide::Output, "out", "float");
  const int64_t ho = addSocket(*h.def, SocketSide::Output, "out", "float");
  const NodeId out = add(*g.def, "graph.output"), x = add(*g.def, "t.sum"), n = add(*g.def, "graph.group");
  const NodeId hout = add(*h.def, "graph.output"), back = add(*h.def, "graph.group");
  test_assert(g.def->setGroup(n, h.id).ok());
  a.lib.writeRef(const_cast<RefCell &>(h.def->node(back)->group()), g.id).ignore();
  connect(*g.def, x, "value", out, key(o));
  test_assert(g.def->insertLink(LinkState{LinkId{5}, x, "value", x, "a"}).ok());
  test_assert(h.def->insertLink(LinkState{LinkId{5}, back, key(o).c_str(), hout, key(ho).c_str()}).ok());
  const Bytes bytes = save(a.lib);
  load(b.ctx, bytes);
  const Graph root = byName(b.lib, "root");
  test_assert(sameGraph(*g.def, *root.def) && same(bytes, save(b.lib)));
  GraphInstance inst(&b.rt, &b.lib, root.id);
  test_assert(hasCode(inst.evaluate(SocketPath::output("out")), StatusCode::FailedPrecondition));
  test_assert(hasCode(inst.evaluate(SocketPath::of(n, key(ho))), StatusCode::FailedPrecondition));
  test_assert(root.def->node(x)->linksIn().size() == 1);
}

/* ---- copies ---- */

void testDuplicate()
{
  IoDoc a;
  Content c(a.lib);
  const BlockHandle both[] = {c.root.h, c.inner.h};
  Result<IdMap> ids = duplicateBlocks(a.lib, both);
  test_assert(ids.ok() && ids->size() == 2);
  GraphDef *root = graphDefOf(a.lib, a.lib.find(ids->find(c.root.id)));
  GraphDef *inner = graphDefOf(a.lib, a.lib.find(ids->find(c.inner.id)));
  test_assert(root->self() == ids->find(c.root.id) && root->serial() != c.root.def->serial());
  test_assert(root->node(c.group)->group().target == inner->self());
  test_assert(root->node(c.each)->group().target == inner->self());
  test_assert(root->node(c.mode)->blockTarget("weak") == inner->self());
  test_assert(sameGraph(*c.inner.def, *inner) && sameGraph(*c.root.def, *root, &*ids));
  GraphInstance inst(&a.rt, &a.lib, root->self()), old(&a.rt, &a.lib, c.root.id);
  const SocketPath p = SocketPath::output("out");
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == kResult && old.evaluate(p).ok());
  /* They are apart. */
  test_assert(root->setParam(c.mode, "mode", PropValue::ofString("KEEP")).ok());
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == -kResult);
  test_assert(old.evaluate(p).ok() && old.value(p)->asFloat() == kResult && old.totalRuns() == 3);
  /* One alone shares what it uses. */
  const BlockHandle one[] = {c.root.h};
  ids = duplicateBlocks(a.lib, one);
  test_assert(ids.ok());
  root = graphDefOf(a.lib, a.lib.find(ids->find(c.root.id)));
  test_assert(root->node(c.group)->group().target == c.inner.id && sameGraph(*c.root.def, *root));
}

void testAppend()
{
  IoDoc a;
  Content c(a.lib);
  const Bytes bytes = save(a.lib);
  Result<AppendResult> r = appendLibrary(a.ctx, view(bytes));
  test_assert(r.ok() && r->ids.size() == 2 && r->report.issues.total == 0);
  GraphDef *root = graphDefOf(a.lib, a.lib.find(r->ids.find(c.root.id)));
  test_assert(root && root->self() == r->ids.find(c.root.id) && root->self() != c.root.id);
  test_assert(root->node(c.group)->group().target == r->ids.find(c.inner.id));
  test_assert(sameGraph(*c.root.def, *root, &r->ids));
  GraphInstance inst(&a.rt, &a.lib, root->self());
  const SocketPath p = SocketPath::output("out");
  test_assert(inst.evaluate(p).ok() && inst.value(p)->asFloat() == kResult);
  /* An edit of the copy says it is the copy's. */
  int told = 0;
  const ScopedConnection connection = a.rt.ext<ChangeBus>().addListener(
      [&](const OwnerKey *owner) { told += owner && owner->block == root->self(); });
  test_assert(root->setLiteral(c.sum, "b", PropValue::ofFloat(1)).ok());
  test_assert(told > 0);
}

/* What a load may take counts what graphs take: nodes, links, sockets and the interface.
 * Each graph here has many of one. */
void testLimits()
{
  for (int kind = 0; kind < 4; kind++) {
    Doc a;
    const Graph g = newGraph(a.lib, "g"), empty = newGraph(a.lib, "empty");
    a.lib.setFlag(g.h, BlockFlags::Root, true);
    a.lib.setFlag(empty.h, BlockFlags::Root, true);
    NodeState s;
    s.id = NodeId{1};
    s.typeName = "t.unknown";
    const int n = 4000;
    for (int i = 0; i < n; i++) {
      const std::string name = std::to_string(i);
      if (kind == 0) {
        s.id = NodeId{i + 1};
        test_assert(g.def->insertNode(s).ok());
      }
      else if (kind == 1) {
        if (i == 0) {
          test_assert(g.def->insertNode(s).ok());
        }
        const LinkState l{LinkId{i + 2}, s.id, "o", s.id, name.c_str()};
        test_assert(g.def->insertLink(l).ok());
      }
      else if (kind == 2) {
        s.inputs.append(SocketInfo{name.c_str(), "", "t"});
      }
      else {
        addSocket(*g.def, SocketSide::Output, name.c_str(), "float");
      }
    }
    if (kind == 2) {
      test_assert(g.def->insertNode(s).ok());
    }
    const Bytes bytes = save(a.lib);
    /* Smaller than what is counted for `kind`, larger than all the rest. */
    const uint64_t counted[] = {sizeof(GraphNode) + 256, sizeof(GraphLink) + 256,
                                2 * sizeof(SocketInfo), 2 * sizeof(InterfaceSocket)};
    for (const bool enough : {false, true}) {
      Doc b;
      LoadOptions options;
      options.limits.maxBytes = enough ? 32 << 20 : n * counted[kind] - 1;
      Result<LoadReport> r = loadLibrary(b.ctx, view(bytes), options);
      test_assert(r.ok() && r->missingType == (enough ? 0 : 1) && byName(b.lib, "empty").def);
      test_assert(enough || hasCode(r->issues.first[0].why, StatusCode::OutOfRange));
      test_assert(same(save(b.lib), bytes));
    }
  }
}

/* Equal names with the same id, for the registries. */
void testNames()
{
  Doc d;
  const char *one = kTwinNodes[0], *other = kTwinNodes[1];
  test_assert(TypeId::of(std::string("node:") + one) == TypeId::of(std::string("node:") + other));
  NodeTypeDef t(one);
  t.output("value", "float");
  addType<modeKernel>(d.rt, t, one);
  test_assert(d.rt.ext<NodeTypeRegistry>().find(one) && !d.rt.ext<NodeTypeRegistry>().find(other));
  test_assert(hasCode(newGraph(d.lib, "g").def->addNode(other), StatusCode::NotFound));
  SocketTypeRegistry &sockets = d.rt.ext<SocketTypeRegistry>();
  test_assert(sockets.addPayloadType<Twin>().ok());
  test_assert(sockets.find(kTwinSockets[0]) && !sockets.find(kTwinSockets[1]));
}

} // namespace

int main()
{
  testRoundTrip();
  testEdits();
  testMissingTypes();
  testStrength();
  testVersions();
  testExtraRefs();
  testEnums();
  testTargets();
  testCycles();
  testDuplicate();
  testAppend();
  testLimits();
  testNames();
  return test_end();
}

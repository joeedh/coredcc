/* Payloads of graphs that are wrong: refused, without leaks or crashes. And random graphs, saved
 * and loaded. */
#include "edit_fixture.h"
#include "foreach_fixture.h"
#include "fuzz.h"
#include "io_compare.h"

#include "coredcc/api/library_io.h"
#include "coredcc/graph/def_io.h"
#include "coredcc/graph/instance.h"
#include "coredcc/lib/payload_access.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

using namespace fixture;

test_init;

namespace {

struct Fields {
  Library *lib;
  BlockWriter out;
  explicit Fields(Library &l) : lib(&l), out(l) {}
  BytesWriter &b()
  {
    return out.bytes();
  }
  void count(uint32_t n)
  {
    b().u32(n);
  }
  void entry(const char *name, PropValue v)
  {
    BlockEntry e;
    e.name = name;
    e.value = std::move(v);
    out.entry(e);
  }
  void socket(const char *key, const char *type)
  {
    b().str(key);
    b().str(key);
    b().str(type);
  }
  void iface(int64_t id, const char *name, const char *type)
  {
    b().i64(id);
    b().str(name);
    b().str(type);
  }
  void noInterface()
  {
    count(0);
    count(0);
  }
  void head(int64_t id, const char *type, uint32_t version = 1)
  {
    b().i64(id);
    b().str(type);
    b().u32(version);
    b().str("label");
    b().f64(1);
    b().f64(2);
  }
  /* A node with nothing. */
  void node(int64_t id, const char *type, uint32_t version = 1)
  {
    head(id, type, version);
    for (int i = 0; i < 6; i++) {
      count(0);
    }
  }
  void link(int64_t id, int64_t from, int64_t to, const char *key)
  {
    b().i64(id);
    b().i64(from);
    b().str("value");
    b().i64(to);
    b().str(key);
  }
  Status read()
  {
    Result<BlockPayload> p = detail::PayloadAccess::take(out, kGraphDefPayload);
    test_assert(p.ok());
    return readInto(*lib, *p);
  }
  static Status readInto(Library &lib, const BlockPayload &p)
  {
    const BlockId id{ClientId::of(kCoredccClient), lib.runtime().newLocalId()};
    Result<BlockHandle> h = lib.createWithId(TypeId::of(kGraphDefType), id, "read", nullptr);
    test_assert(h.ok());
    const Status s = readPayload(lib, *h, p);
    GraphDef *def = graphDefOf(lib, *h);
    test_assert(s.ok() == (def != nullptr));
    if (def) {
      test_assert(def->validate().ok());
    }
    return s;
  }
};

struct HostileDoc : Doc {
  HostileDoc()
  {
    NodeTypeDef t("t.sum");
    t.input(PropertyDef::floatProp("a")).input(PropertyDef::floatProp("b")).output("value", "float");
    addType<sumKernel>(rt, t, "t.sum");
    lib.setQuarantine(false);
    /* Blocks without data are what this test makes. */
    lib.setAuditRelations(false);
  }
};

bool refused(const Status &s, const char *why, StatusCode code = StatusCode::InvalidArgument)
{
  if (s.code() != code || !strstr(s.message(), why)) {
    printf("expected '%s', got %s\n", why, s.toString().c_str());
    return false;
  }
  return true;
}

void testRules()
{
  HostileDoc d;
  /* Checks that what they vary is fine otherwise. */
  auto with = [&](auto &&fn) {
    Fields f(d.lib);
    fn(f);
    return f.read();
  };
  auto nodes = [&](uint32_t n, uint32_t links, auto &&fn) {
    return with([&](Fields &f) {
      f.noInterface();
      f.count(n);
      fn(f);
      if (links == 0) {
        f.count(0);
      }
    });
  };
  test_assert(with([](Fields &f) {
                f.noInterface();
                f.count(0);
                f.count(0);
              }).ok());

  /* ids */
  for (const int64_t id : {int64_t(0), int64_t(-1), int64_t(1) << 52}) {
    test_assert(refused(nodes(1, 0, [&](Fields &f) { f.node(id, "t.sum"); }), "isn't in 1 to"));
  }
  test_assert(nodes(1, 0, [&](Fields &f) { f.node((int64_t(1) << 52) - 1, "t.sum"); }).ok());
  auto two = [&](int64_t a, int64_t b) {
    return nodes(2, 0, [&](Fields &f) {
      f.node(a, "t.sum");
      f.node(b, "t.sum");
    });
  };
  test_assert(two(1, 2).ok() && refused(two(3, 3), "an id that is there twice"));
  auto linked = [&](int64_t id, int64_t from, int64_t to, const char *second = nullptr) {
    return nodes(2, 1, [&](Fields &f) {
      f.node(1, "t.sum");
      f.node(2, "t.other");
      f.count(second ? 2 : 1);
      f.link(id, from, to, "a");
      if (second) {
        f.link(id + 1, from, to, second);
      }
    });
  };
  test_assert(linked(5, 1, 2).ok() && linked(5, 1, 1).ok() && linked(5, 1, 2, "b").ok());
  test_assert(refused(linked(1, 1, 2), "an id that is there twice"));
  test_assert(refused(linked(5, 1, 3), "a node that isn't there"));
  test_assert(refused(linked(5, 3, 1), "a node that isn't there"));
  test_assert(refused(linked(5, 1, 2, "a"), "an input with two links"));

  /* the interface */
  auto interface2 = [&](int64_t id, const char *name, int64_t id2, const char *name2, bool out) {
    return with([&](Fields &f) {
      f.count(out ? 1 : 2);
      f.iface(id, name, "float");
      f.entry("", PropValue::ofFloat(1));
      if (out) {
        f.count(1);
      }
      f.iface(id2, name2, "float");
      if (!out) {
        f.entry("", PropValue());
        f.count(0);
      }
      f.count(1);
      f.node(90, "graph.input");
      f.count(0);
    });
  };
  for (const bool out : {false, true}) {
    test_assert(interface2(1, "a", 2, "b", out).ok());
    test_assert(refused(interface2(1, "a", 1, "b", out), "an id that is there twice"));
    test_assert(refused(interface2(1, "a", 90, "b", out), "an id that is there twice"));
    for (const char *name : {"", "#1", "@1.a", "#"}) {
      test_assert(refused(interface2(1, "a", 2, name, out), "a name interface sockets can't have"));
    }
    test_assert(interface2(1, "a", 2, "a#1@", out).ok());
  }
  test_assert(refused(interface2(1, "a", 2, "a", false), "an interface name that is there twice"));
  test_assert(interface2(1, "a", 2, "a", true).ok());
  test_assert(refused(with([](Fields &f) {
                        f.count(1);
                        f.iface(1, "a", "float");
                        f.count(0);
                        f.count(0);
                        f.count(0);
                        f.count(0);
                      }),
                      "without its default"));
  test_assert(refused(nodes(2, 0, [](Fields &f) {
                        f.node(1, "graph.output");
                        f.node(2, "graph.output");
                      }),
                      "a second graph.output"));

  /* values */
  auto values = [&](bool literals, std::vector<BlockEntry> list) {
    return nodes(1, 0, [&](Fields &f) {
      f.head(1, "t.unknown");
      if (literals) {
        f.count(0);
      }
      f.out.entries(list);
      if (literals) {
        for (size_t i = 0; i < list.size(); i++) {
          f.b().boolean(i == 0);
        }
      }
      for (int i = 0; i < (literals ? 4 : 5); i++) {
        f.count(0);
      }
    });
  };
  auto e = [](const char *name, PropValue v) {
    BlockEntry out;
    out.name = name;
    out.value = std::move(v);
    return out;
  };
  for (const bool literals : {false, true}) {
    test_assert(values(literals, {e("a", PropValue::ofInt(1)), e("b", PropValue::ofInt(1))}).ok());
    test_assert(refused(values(literals, {e("a", PropValue::ofInt(1)), e("a", PropValue::ofInt(2))}),
                        "a value that is there twice"));
    test_assert(refused(values(literals, {e("a", PropValue())}), "a value that isn't set"));
    BlockEntry unknown;
    unknown.name = "new";
    unknown.unknownTag = 200;
    unknown.raw.append(1);
    test_assert(refused(values(literals, {unknown}), "isn't known", StatusCode::Unsupported));
    StableRef runtimeRef;
    runtimeRef.owner = OwnerKey::ofRuntime(1);
    runtimeRef.type = StructId::of("X");
    test_assert(refused(values(literals, {e("a", PropValue::ofPointer(runtimeRef))}), "a pointer among"));
    BlockEntry cell = e("a", PropValue::ofInt(1));
    cell.refs.append(RefCell(RefKind::OptStrong));
    test_assert(refused(values(literals, {cell}), "a pointer among"));
  }
  const std::vector<BlockEntry> types = {e("a", PropValue::ofInt(1)), e("a", PropValue::ofFloat(2))};
  test_assert(values(true, types).ok() && refused(values(false, types), "a value that is there twice"));

  /* references */
  auto references = [&](std::initializer_list<const char *> names, uint32_t row, bool add) {
    return nodes(1, 0, [&](Fields &f) {
      f.head(1, "t.unknown");
      f.count(0);
      f.count(0);
      f.count(uint32_t(names.size()));
      for (const char *name : names) {
        f.b().str(name);
        f.b().boolean(false);
        if (add) {
          f.out.ref(RefCell(RefKind::OptStrong));
        }
        else {
          f.b().u32(row);
        }
      }
      for (int i = 0; i < 3; i++) {
        f.count(0);
      }
    });
  };
  test_assert(references({"a", "b"}, 0, true).ok());
  test_assert(refused(references({"a", "a"}, 0, true), "a reference that is there twice"));
  test_assert(refused(references({"a"}, 0, false), "outside the block's ref table"));

  /* extras: anything, once */
  auto extras = [&](std::vector<BlockEntry> list) {
    return nodes(1, 0, [&](Fields &f) {
      f.head(1, "t.unknown");
      for (int i = 0; i < 3; i++) {
        f.count(0);
      }
      f.out.entries(list);
      f.count(0);
      f.count(0);
    });
  };
  BlockEntry unknown = e("new", PropValue()), cell = e("cell", PropValue::ofInt(1));
  unknown.unknownTag = 200;
  unknown.raw.append(1);
  cell.refs.append(RefCell(RefKind::OptStrong));
  test_assert(extras({e("a", PropValue::ofInt(1)), unknown, cell, e("none", PropValue())}).ok());
  test_assert(refused(extras({unknown, cell, unknown}), "an extra that is there twice"));
  test_assert(refused(extras({e("", PropValue::ofInt(1))}), "an extra without a name"));

  /* versions */
  for (const char *type : {"t.unknown", "t.sum"}) {
    test_assert(nodes(1, 0, [&](Fields &f) { f.node(1, type, 0xffffffff); }).ok());
    test_assert(refused(nodes(1, 0, [&](Fields &f) { f.node(1, type, 0); }), "version 0"));
  }

  /* sockets last seen */
  auto seen = [&](const char *key, bool input) {
    return nodes(1, 0, [&](Fields &f) {
      f.head(1, "t.unknown");
      for (int i = 0; i < (input ? 4 : 5); i++) {
        f.count(0);
      }
      f.count(1);
      f.socket(key, "float");
      if (input) {
        f.count(0);
      }
    });
  };
  for (const bool input : {false, true}) {
    test_assert(seen("a", input).ok() && seen("#", input).ok());
    test_assert(refused(seen("", input), "a socket without a key"));
  }

  /* many that share one row of the ref table */
  for (const bool enough : {false, true}) {
    Fields f(d.lib);
    f.noInterface();
    f.count(1);
    f.head(1, "t.unknown");
    f.count(0);
    f.count(0);
    f.count(4000);
    for (int i = 0; i < 4000; i++) {
      f.b().str(std::to_string(i));
      f.b().boolean(false);
      if (i == 0) {
        f.out.ref(RefCell(RefKind::OptStrong));
      }
      else {
        f.b().u32(0);
      }
    }
    for (int i = 0; i < 4; i++) {
      f.count(0);
    }
    Result<BlockPayload> p = detail::PayloadAccess::take(f.out, kGraphDefPayload);
    test_assert(p.ok());
    uint64_t budget = enough ? 1 << 20 : 4000 * 2 * sizeof(NodeBlockRef) - 1;
    BlockReader in(d.lib, *p);
    detail::PayloadAccess::setBudget(in, &budget);
    GraphDef *def = readGraphDef(in, BlockId{ClientId::of(kCoredccClient), d.rt.newLocalId()});
    test_assert((def != nullptr) == enough);
    test_assert(enough || hasCode(in.bytes().status(), StatusCode::OutOfRange));
    if (def) {
      litestl::alloc::Delete(def);
    }
  }

  /* a block without data */
  Result<BlockHandle> empty = d.lib.create(TypeId::of(kGraphDefType), "empty", nullptr);
  test_assert(empty.ok() && hasCode(writePayload(d.lib, *empty), StatusCode::FailedPrecondition));
  BlockPayload newer;
  newer.version = kGraphDefPayload + 1;
  test_assert(hasCode(readPayload(d.lib, *empty, newer), StatusCode::Unsupported));

  /* counts past the end */
  test_assert(!with([](Fields &f) { f.count(0xffffffff); }).ok());
  test_assert(!with([](Fields &f) {
                 f.noInterface();
                 f.count(0x10000000);
               }).ok());
  test_assert(!nodes(1, 1, [](Fields &f) {
                 f.node(1, "t.sum");
                 f.count(0x7fffffff);
               }).ok());
  /* more than there is */
  test_assert(!with([](Fields &f) {
                 f.noInterface();
                 f.count(0);
                 f.count(0);
                 f.b().u8(0);
               }).ok());
}

/* Names that have the id of others. */
void testNames()
{
  const char *const *node = kTwinNodes, *const *socket = kTwinSockets;
  for (const bool registered : {false, true}) {
    HostileDoc d;
    if (registered) {
      NodeTypeDef t(node[0]);
      t.output("value", "float");
      addType<sumKernel>(d.rt, t, node[0]);
      test_assert(d.rt.ext<SocketTypeRegistry>().addPayloadType<Twin>().ok());
    }
    auto nodeNamed = [&](const char *name) {
      Fields f(d.lib);
      f.noInterface();
      f.count(1);
      f.node(1, name);
      f.count(0);
      return f.read();
    };
    auto ifaceOf = [&](const char *type) {
      Fields f(d.lib);
      f.count(0);
      f.count(1);
      f.iface(1, "a", type);
      f.count(0);
      f.count(0);
      return f.read();
    };
    auto snapshotOf = [&](const char *type) {
      Fields f(d.lib);
      f.noInterface();
      f.count(1);
      f.head(1, "t.unknown");
      f.count(0);
      f.count(0);
      f.count(0);
      f.count(0);
      f.count(0);
      f.count(1);
      f.socket("a", type);
      f.count(0);
      return f.read();
    };
    if (!registered) { /* the first one is kept, also from another block */
      test_assert(nodeNamed(node[0]).ok() && ifaceOf(socket[0]).ok());
    }
    test_assert(refused(nodeNamed(node[1]), "the id of another") && nodeNamed(node[0]).ok());
    test_assert(refused(ifaceOf(socket[1]), "the id of another") && ifaceOf(socket[0]).ok());
    test_assert(refused(snapshotOf(socket[1]), "the id of another") && snapshotOf(socket[0]).ok());
    /* A node's name isn't a socket's. */
    test_assert(nodeNamed(socket[1]).ok() && ifaceOf(node[1]).ok());
    /* A full table refuses nothing. */
    std::string longName(size_t(1) << 20, 'x');
    for (int i = 0; i < 20; i++) {
      d.lib.fileNames().add(IdDomain::Type, longName + std::to_string(i));
    }
    test_assert(nodeNamed("t.another").ok() && nodeNamed("t.another").ok());
  }
}

/* ---- random ---- */

/* A document of three graphs, made by random edits. */
struct RandomDoc : HostileDoc {
  std::mt19937_64 gen;
  Graph g[3];

  int below(int n)
  {
    return int(gen() % uint64_t(n));
  }
  explicit RandomDoc(uint64_t seed) : gen(seed)
  {
    const char *names[] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
      g[i] = newGraph(lib, names[i]);
      lib.setFlag(g[i].h, BlockFlags::Root, true);
    }
    const char *types[] = {"t.sum", "value.int", "value.float", "graph.group", "graph.foreach",
                           "graph.input", "graph.output", "math.float"};
    const char *sockets[] = {"float", "int", "elements", "string"};
    for (int i = 0; i < 250; i++) {
      const int at = below(3);
      GraphDef &d = *g[at].def;
      switch (below(8)) {
        case 0: {
          const SocketSide side = below(2) ? SocketSide::Input : SocketSide::Output;
          d.addInterface(side, std::to_string(below(6)), sockets[below(4)]).status().ignore();
          break;
        }
        case 1:
        case 2:
          d.addNode(types[below(8)]).status().ignore();
          break;
        case 3: {
          NodeState s;
          s.id = NodeId{int64_t(gen() >> 13) + 1};
          s.typeName = below(2) ? "t.unknown" : "t.sum";
          s.inputs.append(SocketInfo{"a", "a", "float"});
          if (below(2)) {
            s.params.append(NodeValue{"p", PropValue::ofString("x")});
          }
          s.refs.append(NodeState::Ref{"r", below(2) ? g[below(3)].id : BlockId(), below(2) == 0});
          d.insertNode(s).ignore();
          break;
        }
        default: {
          if (d.nodeCount() == 0) {
            break;
          }
          const GraphNode &n = *d.nodeAt(below(d.nodeCount())), &m = *d.nodeAt(below(d.nodeCount()));
          const SocketInfos ins = d.sockets(n, SocketSide::Input);
          const SocketInfos outs = d.sockets(m, SocketSide::Output);
          if (n.type() && hasGroup(n.type()->role) && below(2) && at < 2) {
            d.setGroup(n.id(), g[at + 1 + below(2 - at)].id).ignore();
          }
          else if (ins.size() && below(2)) {
            const std::string k = ins[below(int(ins.size()))].key.c_str();
            d.setLiteral(n.id(), k, below(2) ? PropValue::ofFloat(below(9)) : PropValue::ofInt(1)).ignore();
            if (below(3) == 0) {
              d.useLiteral(n.id(), k, below(2) == 0).ignore();
            }
          }
          else if (ins.size() && outs.size()) {
            d.link({m.id(), outs[below(int(outs.size()))].key.c_str()},
                   {n.id(), ins[below(int(ins.size()))].key.c_str()})
                .status()
                .ignore();
          }
          else if (below(12) == 0) {
            d.removeNode(n.id()).ignore();
          }
          else {
            d.setPosition(n.id(), below(9), -below(9)).ignore();
          }
        }
      }
    }
  }
};

/* As large as a file may make them. They are refused, or loaded, checked and evaluated, in about
 * the time their size takes. */
void testLarge()
{
  const auto start = std::chrono::steady_clock::now();
  HostileDoc d;
  /* A def with `n` sums, and a graph with a group of it and `links` links: in pairs, to an
   * input inside and from an output inside. */
  auto interior = [&](int links) {
    const int n = links / 2;
    const Graph inner = newGraph(d.lib, "inner");
    for (int i = 0; i < n; i++) {
      NodeState s;
      s.id = NodeId{i + 1};
      s.typeName = "t.sum";
      test_assert(inner.def->insertNode(s).ok());
    }
    Fields f(d.lib);
    f.noInterface();
    f.count(uint32_t(n + 2));
    f.head(1, "graph.group");
    for (int i = 0; i < 4; i++) {
      f.count(0);
    }
    RefCell group;
    group.target = inner.id;
    f.out.ref(group);
    f.count(0);
    f.count(0);
    f.node(2, "t.sum");
    for (int i = 0; i < n; i++) {
      f.node(3 + i, "t.sum");
    }
    f.count(uint32_t(n * 2));
    for (int i = 0; i < n; i++) {
      const std::string at = "@" + std::to_string(i + 1);
      f.link(1000000 + i, 2, 1, (at + ".a").c_str());
      f.b().i64(2000000 + i);
      f.b().i64(1);
      f.b().str(at + ".value");
      f.b().i64(3 + i);
      f.b().str("a");
    }
    Result<BlockPayload> p = detail::PayloadAccess::take(f.out, kGraphDefPayload);
    test_assert(p.ok());
    const BlockId id{ClientId::of(kCoredccClient), d.rt.newLocalId()};
    Result<BlockHandle> h = d.lib.createWithId(TypeId::of(kGraphDefType), id, "read", nullptr);
    test_assert(h.ok());
    const Status s = readPayload(d.lib, *h, *p);
    if (!s.ok()) {
      return s;
    }
    GraphDef &def = *graphDefOf(d.lib, *h);
    test_assert(def.node(NodeId{1})->group().target == inner.id);
    test_assert(def.setLiteral(NodeId{2}, "a", PropValue::ofFloat(3)).ok());
    test_assert(def.validate().ok() && def.diagnostics().size() == 0);
    test_assert(writePayload(d.lib, *h).ok());
    GraphInstance i(&d.rt, &d.lib, id);
    for (int k = 0; k < n; k += 100) {
      test_assert(i.evaluate(SocketPath::of(NodeId{3 + k}, "value")).ok());
      test_assert(*i.value(SocketPath::of(NodeId{3 + k}, "value")) == GraphValue::ofFloat(3));
    }
    return s;
  };
  test_assert(interior(GraphDef::kMaxInteriorLinks).ok());
  test_assert(refused(interior(GraphDef::kMaxInteriorLinks + 2), "too many links to sockets"));
  test_assert(refused(interior(80000), "too many links to sockets"));

  auto interface = [&](int inputs, int outputs) {
    Fields f(d.lib);
    f.count(uint32_t(inputs));
    for (int i = 0; i < inputs; i++) {
      f.iface(10 + i, ("i" + std::to_string(i)).c_str(), "float");
      f.entry("", PropValue());
    }
    f.count(uint32_t(outputs));
    for (int i = 0; i < outputs; i++) {
      f.iface(100000 + i, ("o" + std::to_string(i)).c_str(), "float");
    }
    f.count(2);
    f.node(1, "graph.input");
    f.node(2, "graph.output");
    f.count(uint32_t(std::min(inputs, outputs)));
    for (int i = 0; i < std::min(inputs, outputs); i++) {
      f.b().i64(200000 + i);
      f.b().i64(1);
      f.b().str("#" + std::to_string(10 + i));
      f.b().i64(2);
      f.b().str("#" + std::to_string(100000 + i));
    }
    return f.read();
  };
  const int most = GraphDef::kMaxInterface;
  test_assert(interface(most, most).ok());
  test_assert(refused(interface(most + 1, 0), "too many interface sockets"));
  test_assert(refused(interface(0, most + 1), "too many interface sockets"));
  test_assert(refused(interface(200000, 0), "too many interface sockets"));
  /* What an unknown node says its sockets are. */
  auto sockets = [&](int n) {
    Fields f(d.lib);
    f.noInterface();
    f.count(1);
    f.head(1, "t.unknown");
    for (int i = 0; i < 4; i++) {
      f.count(0);
    }
    f.count(uint32_t(n));
    for (int i = 0; i < n; i++) {
      f.socket(("s" + std::to_string(i)).c_str(), "float");
    }
    f.count(0);
    f.count(0);
    return f.read();
  };
  test_assert(sockets(most + 1).ok() && refused(sockets(most + 2), "too many sockets"));
  const std::chrono::duration<double> t = std::chrono::steady_clock::now() - start;
  if (t.count() >= 20) {
    printf("the large graphs took %g s\n", t.count());
  }
  test_assert(t.count() < 20);
}

void testRandom(int seeds)
{
  int nodes = 0, links = 0, pulls = 0, damaged = 0, accepted = 0;
  for (int seed = 1; seed <= seeds; seed++) {
    RandomDoc a{uint64_t(seed)};
    HostileDoc b;
    Result<litestl::util::Vector<uint8_t>> bytes = saveLibrary(a.lib);
    test_assert(bytes.ok());
    const std::span<const uint8_t> all(bytes->data(), size_t(bytes->size()));
    Result<LoadReport> r = loadLibrary(b.ctx, all);
    test_assert(r.ok() && r->issues.total == 0);
    for (const Graph &g : a.g) {
      const GraphDef *d = graphDefOf(b.lib, b.lib.find(g.id));
      test_assert(d && sameGraph(*g.def, *d) && d->validate().ok());
      nodes += d->nodeCount();
      links += d->linkCount();
      GraphInstance x(&a.rt, &a.lib, g.id), y(&b.rt, &b.lib, g.id);
      g.def->forEachNode([&](const GraphNode &n) {
        for (const SocketInfo &s : g.def->sockets(n, SocketSide::Output)) {
          const SocketPath p = SocketPath::of(n.id(), s.key.c_str());
          const Status u = x.evaluate(p), v = y.evaluate(p);
          test_assert(u.code() == v.code() && (!u.ok() || *x.value(p) == *y.value(p)));
          pulls++;
        }
      });
    }
    Result<litestl::util::Vector<uint8_t>> again = saveLibrary(b.lib);
    test_assert(again.ok() && again->size() == bytes->size() &&
                memcmp(again->data(), bytes->data(), size_t(bytes->size())) == 0);

    /* The payload, cut short and with bytes changed. */
    Result<BlockPayload> p = writePayload(a.lib, a.g[0].h);
    test_assert(p.ok());
    for (int i = 0; i < 60; i++) {
      BlockPayload q = *p;
      if (i % 3 == 0 || q.bytes.size() == 0) {
        const int keep = a.below(int(q.bytes.size()) + 1);
        q.bytes.resize(size_t(keep));
        const Status s = Fields::readInto(b.lib, q);
        test_assert(s.ok() == (keep == int(p->bytes.size())));
      }
      else {
        for (int n = a.below(3); n >= 0; n--) {
          q.bytes[a.below(int(q.bytes.size()))] ^= uint8_t(1 << a.below(8));
        }
        damaged++;
        accepted += Fields::readInto(b.lib, q).ok();
      }
    }
    /* What was accepted is a graph like any other. */
    std::vector<BlockId> ids;
    b.lib.forEach([&](BlockHandle h, const Datablock &block) {
      if (graphDefOf(b.lib, h)) {
        ids.push_back(block.id());
      }
    });
    for (const BlockId &id : ids) {
      GraphInstance x(&b.rt, &b.lib, id);
      GraphDef &d = *graphDefOf(b.lib, b.lib.find(id));
      std::vector<SocketPath> paths;
      d.forEachNode([&](const GraphNode &n) {
        for (const SocketInfo &s : d.sockets(n, SocketSide::Output)) {
          paths.push_back(SocketPath::of(n.id(), s.key.c_str()));
        }
      });
      for (const SocketPath &path : paths) {
        x.evaluate(path).ignore();
      }
      d.diagnostics();
      test_assert(d.validate().ok() && writePayload(b.lib, b.lib.find(id)).ok());
    }
  }
  printf("%d seeds: %d nodes, %d links, %d pulls, %d damaged payloads (%d accepted)\n", seeds, nodes,
         links, pulls, damaged, accepted);
}

} // namespace

int main(int argc, char **argv)
{
  testRules();
  testNames();
  testLarge();
  testRandom(argc > 1 ? atoi(argv[1]) : 30 * coredcc_test::testScale());
  return test_end();
}

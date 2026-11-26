#pragma once

/* The Runtime, defs, kernels and late registrations of test_graph_random.cc, with what
 * the oracle (naive_eval.h) computes for each kernel, and what the test keeps of an
 * instance. */
#include "edit_fixture.h"
#include "naive_eval.h"

#include "fuzz.h"

#include <cmath>
#include <cstdio>

namespace random_graph {

using namespace fixture;
using coredcc_test::SplitMix64;

struct P {
  int n = 0;
};
struct Q {
  int n = 0;
};

} // namespace random_graph

template <> struct coredcc::PayloadTraits<random_graph::P> {
  static constexpr const char *name = "r.P";
};
template <> struct coredcc::PayloadTraits<random_graph::Q> {
  static constexpr const char *name = "r.Q";
};

namespace random_graph {

using naive::BlockRead;
using naive::Environment;
using naive::Value;

/* ---- kernels, and what the oracle computes for them ---- */

inline double wrap(double v)
{
  return std::fmod(std::fabs(v), 16);
}
inline Status fail(StatusCode code)
{
  return Status::error(code, "on purpose");
}

inline Status sumKernel(NodeExecContext &ctx)
{
  const double r = wrap(ctx.input(0).asFloat() +
                        ctx.input(1).asFloat() * double(ctx.param("gain")->asInt()));
  return r == 7 ? fail(StatusCode::OutOfRange) : ctx.setOutput(0, GraphValue::ofFloat(r));
}
inline Status splitKernel(NodeExecContext &ctx)
{
  const int64_t n = ctx.input(0).asInt();
  CDCC_TRY(ctx.setOutput(0, GraphValue::ofInt(n / 2)));
  return ctx.setOutput(1, GraphValue::ofBool(n % 2 != 0));
}
inline Status blockKernel(NodeExecContext &ctx)
{
  const BlockUse u = ctx.paramBlock("thing");
  const Thing *t = static_cast<const Thing *>(u.data);
  return ctx.setOutput(0,
                       GraphValue::ofFloat(t ? wrap(ctx.input(0).asFloat() + t->value)
                                             : (u.handle ? 14 : 15)));
}
inline Status vecKernel(NodeExecContext &ctx)
{
  const double *c = ctx.input(0).components();
  return ctx.setOutput(0, GraphValue::ofFloat(wrap(c[0] + c[1] + c[2])));
}
inline Status makeVecKernel(NodeExecContext &ctx)
{
  const double x = ctx.input(0).asFloat();
  return ctx.setOutput(0, GraphValue::ofVec3(x, wrap(x + 1), 0));
}
inline Status textKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0, GraphValue::ofString(ctx.param("text")->asString().c_str()));
}
inline Status payKernel(NodeExecContext &ctx)
{
  return ctx.setOutput(0,
                       GraphValue::ofPayload(makePayload(P{int(ctx.input(0).asInt())})));
}
inline Status countKernel(NodeExecContext &ctx)
{
  const P *p = ctx.input(0).payload<P>();
  return ctx.setOutput(0, GraphValue::ofInt(p ? p->n : -1));
}
inline Status lateKernel(NodeExecContext &ctx)
{
  CDCC_TRY(ctx.setOutput(0, GraphValue::ofFloat(wrap(ctx.input(0).asFloat() * 2))));
  return ctx.setOutput(1, GraphValue::ofPayload(makePayload(Q{3})));
}

inline Status vecToFloat(const GraphValue &in, GraphValue &out)
{
  if (in.components()[0] == 3) {
    return fail(StatusCode::Unsupported);
  }
  out = GraphValue::ofFloat(in.components()[0]);
  return Status();
}
inline Status textToFloat(const GraphValue &in, GraphValue &out)
{
  out = GraphValue::ofFloat(double(in.asString().size()));
  return Status();
}
inline Status payToInt(const GraphValue &in, GraphValue &out)
{
  const P *p = in.payload<P>();
  if (!p) {
    return fail(StatusCode::Cancelled);
  }
  out = GraphValue::ofInt(p->n);
  return Status();
}

inline Value num(const char *type, double v)
{
  return Value{type, {v}, ""};
}

/* A null payload. Its number is what r.count says of it. */
inline Value nullOf(const char *type)
{
  return Value{type, {-1}, "null"};
}

inline Value toValue(const GraphValue &v)
{
  switch (v.type()) {
  case GraphValue::Type::Bool:
    return num("bool", v.asBool());
  case GraphValue::Type::Int:
    return num("int", double(v.asInt()));
  case GraphValue::Type::Float:
    return num("float", v.asFloat());
  case GraphValue::Type::Vec3:
    return Value{"vec3", {v.components()[0], v.components()[1], v.components()[2]}, ""};
  case GraphValue::Type::String:
    return Value{"string", {}, v.asString().c_str()};
  case GraphValue::Type::Payload:
    if (const P *p = v.payload<P>()) {
      return num("r.P", p->n);
    }
    if (const Q *q = v.payload<Q>()) {
      return num("r.Q", q->n);
    }
    return nullOf("r.?");
  default:
    return Value();
  }
}

inline bool sameValue(const GraphValue &engine, const Value &oracle)
{
  const Value v = toValue(engine);
  if (v.text == "null") { /* GraphValue doesn't say which type */
    return oracle.text == "null";
  }
  return v == oracle;
}

/* ---- the world ---- */

constexpr int kDefs = 4;
constexpr int kThings = 3;

struct World {
  Runtime rt;
  Library lib;
  Environment env;
  std::vector<Graph> defs;
  std::vector<BlockId> things;
  std::vector<std::pair<std::string, std::string>> lateConversions = {
      {"vec3", "float"}, {"string", "float"}, {"r.P", "int"}};
  bool late = false;
  int names = 0;
  /** Edits go through commands, and are undone and redone. */
  const bool undoable;
  LinearHistory history;
  CommandExecutor executor;
  std::unique_ptr<Context> ctx;

  World(uint64_t seed, bool undo) : lib(&rt), undoable(undo), executor(&rt, &history)
  {
    /* Seeded ids, so that a seed always gives the same run. */
    rt.setGuidSource([ids = SplitMix64(~seed)]() mutable {
      const uint64_t hi = ids.next();
      return LocalId{hi, ids.next()};
    });
    addThings(rt);
    test_assert(registerGraphCommands(rt).ok() && registerBlockCommands(rt).ok());
    addDocument(rt);
    ctx = std::make_unique<Context>(&rt, &lib, rt.ext<ApiRegistry>().runtimeOwner("doc"));
    ctx->setExecutor(&executor);
    lib.setQuarantine(undo);
    lib.setAuditRelations(undo);
    SocketTypeRegistry &sockets = rt.ext<SocketTypeRegistry>();
    test_assert(sockets.addPayloadType<P>().ok());
    env.rt = &rt;
    env.lib = &lib;
    env.types = {{"bool", num("bool", 0)},
                 {"int", num("int", 0)},
                 {"float", num("float", 0)},
                 {"vec3", Value{"vec3", {0, 0, 0}, ""}},
                 {"string", Value{"string", {}, ""}},
                 {"r.P", nullOf("r.P")}};
    builtinConversions();

    NodeTypeDef s("r.sum");
    s.input(PropertyDef::floatProp("a"))
        .input(PropertyDef::floatProp("b").defaultValue(PropValue::ofFloat(2)))
        .param(
            PropertyDef::intProp("gain").range(-2, 3).defaultValue(PropValue::ofInt(1)))
        .output("value", "float");
    addType<sumKernel>(rt, s, "r.sum");
    env.kernels["r.sum"] = [](auto &in, auto &params, auto &, std::vector<Value> &out) {
      const double r =
          wrap(in[0].numbers[0] + in[1].numbers[0] * double(params.at("gain").asInt()));
      out = {num("float", r)};
      return r == 7 ? StatusCode::OutOfRange : StatusCode::Ok;
    };
    NodeTypeDef sp("r.split");
    sp.input(PropertyDef::intProp("n")).output("half", "int").output("odd", "bool");
    addType<splitKernel>(rt, sp, "r.split");
    env.kernels["r.split"] = [](auto &in, auto &, auto &, std::vector<Value> &out) {
      const int64_t n = int64_t(in[0].numbers[0]);
      out = {num("int", double(n / 2)), num("bool", n % 2 != 0)};
      return StatusCode::Ok;
    };
    NodeTypeDef b("r.block");
    b.input(PropertyDef::floatProp("a"))
        .param(PropertyDef::pointerProp("thing", kThingStruct))
        .output("value", "float");
    addType<blockKernel>(rt, b, "r.block");
    env.kernels["r.block"] =
        [](auto &in, auto &, const BlockRead &r, std::vector<Value> &out) {
          const bool live = r.handle && r.state == BlockState::Live;
          out = {num("float",
                     live ? wrap(in[0].numbers[0] + r.value) : (r.handle ? 14 : 15))};
          return StatusCode::Ok;
        };
    NodeTypeDef v("r.vec");
    v.input(PropertyDef::vecProp("v", 3)).output("value", "float");
    addType<vecKernel>(rt, v, "r.vec");
    env.kernels["r.vec"] = [](auto &in, auto &, auto &, std::vector<Value> &out) {
      const auto &c = in[0].numbers;
      out = {num("float", wrap(c[0] + c[1] + c[2]))};
      return StatusCode::Ok;
    };
    NodeTypeDef mv("r.makeVec");
    mv.input(PropertyDef::floatProp("x")).output("v", "vec3");
    addType<makeVecKernel>(rt, mv, "r.makeVec");
    env.kernels["r.makeVec"] = [](auto &in, auto &, auto &, std::vector<Value> &out) {
      const double x = in[0].numbers[0];
      out = {Value{"vec3", {x, wrap(x + 1), 0}, ""}};
      return StatusCode::Ok;
    };
    NodeTypeDef t("r.text");
    t.param(PropertyDef::stringProp("text")).output("s", "string");
    addType<textKernel>(rt, t, "r.text");
    env.kernels["r.text"] = [](auto &, auto &params, auto &, std::vector<Value> &out) {
      out = {Value{"string", {}, params.at("text").asString().c_str()}};
      return StatusCode::Ok;
    };
    NodeTypeDef p("r.pay");
    p.input(PropertyDef::intProp("n")).output("p", "r.P");
    addType<payKernel>(rt, p, "r.pay");
    env.kernels["r.pay"] = [](auto &in, auto &, auto &, std::vector<Value> &out) {
      out = {num("r.P", in[0].numbers[0])};
      return StatusCode::Ok;
    };
    NodeTypeDef c("r.count");
    c.payloadInput("p", "r.P").output("n", "int");
    addType<countKernel>(rt, c, "r.count");
    env.kernels["r.count"] = [](auto &in, auto &, auto &, std::vector<Value> &out) {
      out = {num("int", in[0].numbers[0])};
      return StatusCode::Ok;
    };

    for (int i = 0; i < kDefs; i++) {
      defs.push_back(makeGraph(lib, ("def" + std::to_string(i)).c_str()));
      lib.setFlag(defs.back().h, BlockFlags::Root, true);
    }
    for (int i = 0; i < kThings; i++) {
      things.push_back(idOf(lib, newThing(lib, "thing", i)));
    }
  }

  void builtinConversions()
  {
    const char *numeric[] = {"bool", "int", "float"};
    for (const char *from : numeric) {
      for (const char *to : numeric) {
        if (from != to) {
          const std::string t = to;
          env.conversions[{from, to}] = [t](const Value &in, Value &out) {
            const double x = in.numbers[0];
            out = num(t.c_str(),
                      t == "bool"  ? double(x != 0)
                      : t == "int" ? std::trunc(x)
                                   : x);
            return StatusCode::Ok;
          };
        }
      }
    }
    env.conversions[{"float", "vec3"}] = [](const Value &in, Value &out) {
      const double x = in.numbers[0];
      out = Value{"vec3", {x, x, x}, ""};
      return StatusCode::Ok;
    };
  }

  void registerLateConversion()
  {
    if (lateConversions.empty()) {
      return;
    }
    const auto c = lateConversions.back();
    lateConversions.pop_back();
    SocketTypeRegistry &sockets = rt.ext<SocketTypeRegistry>();
    const SocketConvertFn fn = c.first == "vec3"     ? vecToFloat
                               : c.first == "string" ? textToFloat
                                                     : payToInt;
    test_assert(sockets.addConversion(c.first, c.second, fn).ok());
    env.conversions[c] = [c](const Value &in, Value &out) {
      if (c.first == "string") {
        out = num("float", double(in.text.size()));
        return StatusCode::Ok;
      }
      out = num(c.second.c_str(), in.numbers[0]);
      if (c.first == "vec3") {
        return in.numbers[0] == 3 ? StatusCode::Unsupported : StatusCode::Ok;
      }
      return in.text == "null" ? StatusCode::Cancelled : StatusCode::Ok;
    };
  }

  void registerLateType()
  {
    if (late) {
      return;
    }
    late = true;
    test_assert(rt.ext<SocketTypeRegistry>().addPayloadType<Q>().ok());
    env.types["r.Q"] = nullOf("r.Q");
    NodeTypeDef l("r.late");
    l.input(PropertyDef::floatProp("a")).output("value", "float").output("z", "r.Q");
    addType<lateKernel>(rt, l, "r.late");
    env.kernels["r.late"] = [](auto &in, auto &, auto &, std::vector<Value> &out) {
      out = {num("float", wrap(in[0].numbers[0] * 2)), num("r.Q", 3)};
      return StatusCode::Ok;
    };
  }

  bool live(const Graph &g) const
  {
    const Datablock *b = lib.get(g.h);
    return b && b->state() == BlockState::Live;
  }
  void notify(BlockId id)
  {
    env.notified[id]++;
    rt.ext<ChangeBus>().notify(OwnerKey::ofBlock(id));
  }
};

/* ---- one instance, and what the test knows about it ---- */

using GroupIds = std::vector<int64_t>;

struct Client {
  struct Run {
    std::string type;
    std::vector<Value> inputs;
    std::vector<std::string> sources;
    std::map<std::string, PropValue> params;
    uint64_t paramVersion = 0;
    BlockRead block;
    bool failed = false;
  };
  struct Node {
    std::string type;
    uint64_t runs = 0;
    bool visited = false;
    bool failed = false;
    naive::Error error;
    std::vector<Value> outputs;
    std::vector<int> versions;
    bool hasRun = false;
    Run run;
  };
  struct Last {
    SocketPath path;
    bool failed = false;
    naive::Error error;
    /* The engine's: the oracle has no messages, so only its changes count. */
    std::string message;
    bool hasValue = false;
    Value good;
    uint64_t changedAt = 0;
  };

  /* A prepared socket, and what its handle gave last. */
  struct Prepared {
    SocketPath path;
    SocketHandle handle;
    bool evaluated = false;
    StatusCode code = StatusCode::Ok;
    std::string message;
    GraphValue value;
    uint64_t changedAt = 0;
  };

  int def;
  std::unique_ptr<GraphInstance> inst;
  /* Of the prepared sockets: an instance of its own, as the runs of `inst` are counted.
   */
  std::unique_ptr<GraphInstance> batch;
  std::vector<Prepared> prepared;
  std::map<int64_t, std::pair<Value, int>> external;
  std::map<int64_t, GraphValue> given;
  int externalVersion = 0;
  int versions = 0;
  std::map<std::pair<GroupIds, int64_t>, Node> nodes;
  std::map<GroupIds, uint64_t> frameDefs;
  std::map<std::string, Last> last;
  uint64_t rootSerial = 0;

  void dropBelow(const GroupIds &p)
  {
    auto under = [&p](const GroupIds &q) {
      return q.size() >= p.size() && std::equal(p.begin(), p.end(), q.begin());
    };
    for (auto it = nodes.begin(); it != nodes.end();) {
      it = under(it->first.first) ? nodes.erase(it) : std::next(it);
    }
    for (auto it = frameDefs.begin(); it != frameDefs.end();) {
      it = under(it->first) ? frameDefs.erase(it) : std::next(it);
    }
  }
};

struct Counts {
  uint64_t ops = 0, pulls = 0, runs = 0, musts = 0, mays = 0, visits = 0, errors = 0,
           cycles = 0, groups = 0, recursions = 0, undos = 0, redos = 0, fullPasses = 0,
           forcedPasses = 0, handles = 0, moved = 0;
};
inline Counts g_counts;
inline uint64_t g_seed = 0;
inline int g_batch = 0;

} // namespace random_graph

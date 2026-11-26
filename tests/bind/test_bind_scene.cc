/* Scenes for scripts, through a fake foreign host: transform types with a script's model,
 * what such a model may call, and the scene facades. */
#include "../scene/command_doc.h"
#include "fake_host.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/bind/checked.h"
#include "coredcc/bind/facades.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/foreign_transform.h"
#include "coredcc/bind/graph_facades.h"
#include "coredcc/bind/manager.h"
#include "coredcc/bind/scene_facades.h"

#include <functional>
#include <memory>
#include <new>

using namespace scene_test;
namespace sc = coredcc::script;
using fakehost::Obj;
using litestl::util::string;

test_init;

namespace {

PickOptions near(double radius)
{
  PickOptions o;
  o.radius = radius;
  return o;
}

/* The "JS object" of a model for channels `radius` and `angle`: the location is the polar
 * point in the plane of x and y. */
struct Polar {
  bool throwEvaluate = false, throwSetWorld = false, refuse = false;
  bool hasDelta = false;
  int evaluates = 0, deltas = 0;
  std::function<void()> onEvaluate, onSetWorld;

  static void solve(double x, double y, ChannelBlock &out)
  {
    out.set(0, std::hypot(x, y));
    out.set(1, std::atan2(y, x));
  }
  Obj make()
  {
    Obj o;
    o.methods["evaluate"] = [this](void **args, void *ret, string *err) {
      evaluates++;
      if (onEvaluate) {
        onEvaluate();
      }
      if (throwEvaluate) {
        *err = "evaluate: Error: no state";
        return false;
      }
      const EvalContext &ctx = *static_cast<const EvalContext *>(args[0]);
      const TransformState &parent = *static_cast<const TransformState *>(args[1]);
      const double r = ctx.channel(0), a = ctx.channel(1);
      TransformState *s = new (ret) TransformState();
      /* Parents are only moved. */
      s->world = solve::affine(Linear::identity(),
                               solve::translationOf(parent.world) +
                                   double3(r * std::cos(a), r * std::sin(a), 0));
      s->rotation = parent.rotation;
      return true;
    };
    o.methods["setWorld"] = [this](void **args, void *ret, string *err) {
      if (onSetWorld) {
        onSetWorld();
      }
      if (throwSetWorld) {
        *err = "setWorld: Error: no channels";
        return false;
      }
      const mat4d &target = *static_cast<const mat4d *>(args[1]);
      const TransformState &parent = *static_cast<const TransformState *>(args[2]);
      const double3 p = solve::translationOf(target) - solve::translationOf(parent.world);
      solve(p[0], p[1], *static_cast<ChannelBlock *>(args[4]));
      new (ret)
          Status(refuse ? Status::error(StatusCode::Unsupported, "not there") : Status());
      return true;
    };
    o.methods["applyDelta"] = [this](void **args, void *ret, string *) {
      deltas++;
      const EvalContext &base = *static_cast<const EvalContext *>(args[0]);
      const TransformDelta &d = *static_cast<const TransformDelta *>(args[2]);
      const double r = base.channel(0), a = base.channel(1);
      solve(r * std::cos(a) + d.vector[0],
            r * std::sin(a) + d.vector[1],
            *static_cast<ChannelBlock *>(args[4]));
      new (ret) Status();
      return true;
    };
    return o;
  }
};

TransformTypeInfo *polarType(const char *id)
{
  TransformTypeInfo *type = sc::newTransformTypeDef(id);
  must(sc::transformTypeDefUiName(*type, "Polar"));
  must(sc::transformTypeDefChannel(*type, "radius", sc::makeFloat(2), true));
  must(sc::transformTypeDefChannel(*type, "angle", sc::makeFloat(0), false));
  return type;
}

Status registerPolar(Doc &d,
                     fakehost::Host &host,
                     Polar &model,
                     const char *id,
                     const char *slots = "evaluate setWorld",
                     fakehost::lf::Handle *handle = nullptr)
{
  TransformTypeInfo *type = polarType(id);
  const fakehost::lf::Handle h = host.add(model.make());
  if (handle) {
    *handle = h;
  }
  Status s = sc::registerForeignTransformType(*d.ctx, *type, h, slots);
  litestl::alloc::Delete(type);
  return s;
}

void testRegistration()
{
  fakehost::Host host;
  Polar model;
  {
    CommandDoc d;
    TransformRegistry &reg = d.rt.ext<TransformRegistry>();
    const int tables = d.rt.ifaces().tableCount();
    const int types = reg.count();
    const auto nothingNew = [&] {
      return d.rt.ifaces().tableCount() == tables && reg.count() == types &&
             !d.rt.ext<ApiRegistry>().find(StructId::of("js.a")) && !reg.find("js.a");
    };
    struct Case {
      const char *id;
      const char *slots;
      StatusCode code;
      const char *message;
    };
    const Case cases[] = {
        {"core.x", "evaluate setWorld", StatusCode::InvalidArgument, "reserved"},
        {"core.transform.trs",
         "evaluate setWorld",
         StatusCode::InvalidArgument,
         "reserved"},
        {"", "evaluate setWorld", StatusCode::InvalidArgument, "dotted"},
        {"plain", "evaluate setWorld", StatusCode::InvalidArgument, "dotted"},
        {"a..b", "evaluate setWorld", StatusCode::InvalidArgument, "dotted"},
        {".a", "evaluate setWorld", StatusCode::InvalidArgument, "dotted"},
        {"a.", "evaluate setWorld", StatusCode::InvalidArgument, "dotted"},
        {"a.2", "evaluate setWorld", StatusCode::InvalidArgument, "dotted"},
        {"a.b c", "evaluate setWorld", StatusCode::InvalidArgument, "dotted"},
        {"test.Shape", "evaluate setWorld", StatusCode::AlreadyExists, "test.Shape"},
        {"js.a", "", StatusCode::TypeMismatch, ""},
        {"js.a", "evaluate", StatusCode::TypeMismatch, "setWorld"},
        {"js.a", "evaluate setWorld bogus", StatusCode::TypeMismatch, "bogus"},
    };
    for (const Case &c : cases) {
      fakehost::lf::Handle h = 0;
      const Status s = registerPolar(d, host, model, c.id, c.slots, &h);
      if (!isError(s, c.code, c.message)) {
        fprintf(stderr, "'%s' '%s': %s\n", c.id, c.slots, s.message());
      }
      test_assert(isError(s, c.code, c.message));
      test_assert(host.releasesOf(h) == 1 && nothingNew());
    }

    /* What the type says. */
    const auto refused = [&](StatusCode code, const char *message, auto &&add) {
      TransformTypeInfo *type = polarType("js.a");
      Status s = add(*type);
      if (s.ok()) {
        const fakehost::lf::Handle h = host.add(model.make());
        s = sc::registerForeignTransformType(*d.ctx, *type, h, "evaluate setWorld");
        test_assert(host.releasesOf(h) == 1);
      }
      test_assert(isError(s, code, message) && nothingNew());
      litestl::alloc::Delete(type);
    };
    const auto channel = [&](const char *name, const PropValue &v, bool distance) {
      return [=](TransformTypeInfo &type) {
        return sc::transformTypeDefChannel(type, name, v, distance);
      };
    };
    const StatusCode invalid = StatusCode::InvalidArgument;
    refused(invalid, "two channels", channel("radius", sc::makeInt(1), false));
    refused(invalid, "reserved", channel("length", sc::makeInt(1), false));
    refused(invalid, "reserved", channel("active", sc::makeInt(1), false));
    refused(invalid, "String", channel("s", PropValue::ofString("x"), false));
    refused(invalid, "2^53", channel("i", sc::makeInt(int64_t(1) << 60), false));
    refused(invalid, "distance", channel("i", sc::makeInt(1), true));
    refused(invalid, "distance", channel("b", sc::makeBool(true), true));
    refused(invalid, "m", channel("m", PropValue::identityMat4(), false));
    refused(invalid, "", channel("not a name", sc::makeInt(1), false));
    TransformTypeInfo *type = polarType("js.a");
    test_assert(
        isError(sc::registerForeignTransformType(*d.ctx, *type, 0, "evaluate setWorld"),
                StatusCode::InvalidArgument,
                "handle"));
    {
      const EvaluationScope evaluating(d.rt);
      const fakehost::lf::Handle h = host.add(model.make());
      test_assert(
          isError(sc::registerForeignTransformType(*d.ctx, *type, h, "evaluate setWorld"),
                  StatusCode::Busy));
      test_assert(host.releasesOf(h) == 1 && nothingNew());
    }

    /* A type with every kind, after all that. */
    must(sc::transformTypeDefChannel(*type, "b", sc::makeBool(true), false));
    must(sc::transformTypeDefChannel(*type, "i", sc::makeInt(-3), false));
    must(sc::transformTypeDefChannel(*type, "v", sc::makeVec3(4, 5, 6), true));
    const fakehost::lf::Handle h = host.add(model.make());
    must(sc::registerForeignTransformType(*d.ctx, *type, h, " setWorld  evaluate "));
    litestl::alloc::Delete(type);
    test_assert(host.releasesOf(h) == 0);
    const TransformType *t = reg.find("js.a");
    test_assert(t && t->channelCount() == 7 && !t->model().hasApplyDelta());
    const double want[] = {2, 0, 1, -3, 4, 5, 6};
    const bool distance[] = {true, false, false, false, true, true, true};
    for (int i = 0; i < 7; i++) {
      test_assert(t->defaults().get(i) == want[i] && t->isDistance(i) == distance[i]);
    }
    const ApiStruct *st = d.rt.ext<ApiRegistry>().find(StructId::of("js.a"));
    test_assert(st && st->memberCount() == 5);

    fakehost::lf::Handle again = 0;
    test_assert(
        isError(registerPolar(d, host, model, "js.a", "evaluate setWorld", &again),
                StatusCode::AlreadyExists));
    test_assert(host.releasesOf(again) == 1 && host.releasesOf(h) == 0);
    must(registerPolar(d, host, model, "js.b", "evaluate setWorld applyDelta"));
    test_assert(reg.find("js.b")->model().hasApplyDelta());
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

bool isNear(CommandDoc &d, BlockHandle h, double x, double y, double z)
{
  ObjectEval e;
  return d.sc().evaluated(h, e) &&
         lengthOf(solve::translationOf(e.state.world) - double3(x, y, z)) < 1e-12;
}

std::string textOf(CommandDoc &d, BlockHandle h)
{
  const sc::StringResult r = sc::pointerText(*d.ctx, d.ptr(h));
  test_assert(r.status.ok());
  return r.value.c_str();
}

bool isText(const string &s, const std::string &want)
{
  return want == s.c_str();
}

string str(const std::string &s)
{
  return string(s.c_str());
}

void testModel()
{
  fakehost::Host host;
  Polar model;
  {
    CommandDoc d;
    must(registerPolar(d, host, model, "js.polar"));
    const BlockHandle root = d.object("root", 0, 0, 3);
    PropertyBag out = d.cycle(kAddObjectCommand,
                              {{"type", PropValue::ofString("js.polar")},
                               {"parent", d.ptr(root)},
                               {"location", PropValue::ofVec3(3, 4, 3)}});
    const BlockHandle a = d.blockOut(out, "object");
    test_assert(std::abs(d.obj(a).channels.get(0) - 5.0) < 1e-12);
    d.evaluate();
    test_assert(isNear(d, a, 3, 4, 3));
    const sc::MatrixResult m = sc::worldMatrix(*d.ctx, str(textOf(d, a)));
    test_assert(m.status.ok() &&
                lengthOf(solve::translationOf(m.value) - double3(3, 4, 3)) < 1e-12);
    d.cycle(kApplyObjectDeltaCommand,
            {{"vector", PropValue::ofVec3(1, 1, 0)},
             {"objects",
              PropValue::ofString(
                  sc::blockIdText(*d.ctx, str(textOf(d, a))).value.c_str())}});
    d.evaluate();
    test_assert(isNear(d, a, 4, 5, 3) && model.deltas == 0);

    /* A place it can't be at: the caller's check, not the model's. */
    test_assert(isError(d.fails(kAddObjectCommand,
                                {{"type", PropValue::ofString("js.polar")},
                                 {"parent", d.ptr(root)},
                                 {"location", PropValue::ofVec3(3, 4, 9)}}),
                        StatusCode::FailedPrecondition));
    model.refuse = true;
    test_assert(isError(d.fails(kAddObjectCommand,
                                {{"type", PropValue::ofString("js.polar")},
                                 {"location", PropValue::ofVec3(3, 4, 0)}}),
                        StatusCode::Unsupported,
                        "not there"));
    model.refuse = false;
    model.throwSetWorld = true;
    test_assert(isError(d.fails(kAddObjectCommand,
                                {{"type", PropValue::ofString("js.polar")},
                                 {"location", PropValue::ofVec3(3, 4, 0)}}),
                        StatusCode::Internal,
                        "no channels"));
    model.throwSetWorld = false;

    /* One that throws: the error is returned, its place is its parent's, and the rest is
     * there. */
    const BlockHandle other = d.object("other", 7);
    model.throwEvaluate = true;
    const Status thrown = sc::evaluateScene(*d.ctx);
    test_assert(isError(thrown, StatusCode::Internal, "no state") &&
                !d.rt.hasStickyError());
    ObjectEval e;
    test_assert(d.sc().evaluated(a, e) && e.status == EvalStatus::ModelFailed);
    test_assert(e.state.world[3][2] == 3 && d.isAt(other, 7));
    test_assert(isError(sc::worldMatrix(*d.ctx, str(textOf(d, a))).status,
                        StatusCode::Internal,
                        "no state"));
    model.throwEvaluate = false;
    must(sc::evaluateScene(*d.ctx));
    test_assert(isNear(d, a, 4, 5, 3));

    must(registerPolar(d, host, model, "js.delta", "evaluate setWorld applyDelta"));
    d.select({a});
    d.cycle(kSetTransformTypeCommand, {{"type", PropValue::ofString("js.delta")}});
    d.cycle(kApplyObjectDeltaCommand, {{"vector", PropValue::ofVec3(-1, 0, 0)}});
    d.evaluate();
    test_assert(isNear(d, a, 3, 5, 3) && model.deltas == 1);

    /* The document goes before the Runtime; the model with the Runtime. */
    test_assert(host.live() == 2);
  }
  test_assert(host.live() == 0 && host.allReleasedOnce());
}

View viewOfXy()
{
  View v;
  v.viewport[2] = v.viewport[3] = 100;
  v.view.identity();
  v.proj.identity();
  /* Orthographic, x and y from -10 to 10, z from 10 down to -10. */
  v.proj[0][0] = v.proj[1][1] = 0.1;
  v.proj[2][2] = -0.05;
  v.proj[3][2] = 0.5;
  v.ortho = true;
  return v;
}

/* Everything that takes a Context, from a slot of a model. */
void testGates()
{
  fakehost::Host host;
  Polar model;
  {
    CommandDoc d;
    must(registerPolar(d, host, model, "js.polar"));
    const BlockHandle o = d.object("o");
    d.obj(o).setTransformType("js.polar");
    d.obj(o).channels = d.rt.ext<TransformRegistry>().find("js.polar")->defaults();
    Context &ctx = *d.ctx;
    const string name = str(textOf(d, o)), sceneName = str(textOf(d, d.scene));
    int asked = 0;
    const auto all = [&] {
      asked++;
      const Status seen[] = {
          sc::newScene(ctx, "x").status,
          sc::setScene(ctx, ""),
          sc::setScene(ctx, sceneName),
          sc::blockIdText(ctx, name).status,
          sc::evaluateScene(ctx),
          sc::worldMatrix(ctx, name).status,
          sc::sceneResults(ctx, {}, true).status,
          sc::sceneObjects(ctx, {}, true).status,
          sc::sceneChildren(ctx, "").status,
          sc::pick(ctx, viewOfXy(), 50, 50, near(5)).status,
          sc::getValue(ctx, "scene.time").status,
          sc::setValue(ctx, "scene.time", sc::makeFloat(1)),
          sc::writeValue(ctx, "scene.time", sc::makeFloat(1)),
          sc::resolvePath(ctx, "scene").status,
          sc::pathMeta(ctx, "scene").status,
          sc::invoke(ctx, "core.scene.new").status,
          sc::undo(ctx),
          sc::redo(ctx),
          sc::newGraph(ctx, "g").status,
          sc::newGraphInstance(ctx, "g").status,
      };
      for (const Status &s : seen) {
        if (s.code() != StatusCode::Busy) {
          fprintf(stderr, "in a model: %s\n", s.message());
        }
        test_assert(s.code() == StatusCode::Busy);
      }
    };
    /* As an object of the scene is evaluated, and with no evaluation around. */
    model.onEvaluate = all;
    must(sc::evaluateScene(ctx));
    test_assert(asked == 1);
    test_assert(evaluateTransform(*d.rt.ext<TransformRegistry>().find("js.polar"),
                                  EvalContext(d.obj(o).channels),
                                  TransformState())
                    .ok());
    test_assert(asked == 2);
    model.onEvaluate = nullptr;
    model.onSetWorld = all;
    d.select({o});
    d.ok(kApplyObjectDeltaCommand, {{"vector", PropValue::ofVec3(1, 0, 0)}});
    test_assert(asked == 3 && ctx.scene() == d.scene);
  }
  test_assert(host.allReleasedOnce());
}

/* From a script command's slots: exec may, the others only read. */
void testCommandSlots()
{
  fakehost::Host host;
  {
    CommandDoc d;
    Context &ctx = *d.ctx;
    const BlockHandle o = d.object("o", 1);
    const string name = str(textOf(d, o));
    std::vector<Status> inCanRun, inExec;
    const auto all = [&](std::vector<Status> &seen) {
      seen.push_back(sc::newScene(ctx, "x").status);
      seen.push_back(sc::setScene(ctx, str(textOf(d, d.scene))));
      seen.push_back(sc::blockIdText(ctx, name).status);
      seen.push_back(sc::evaluateScene(ctx));
      seen.push_back(sc::worldMatrix(ctx, name).status);
      seen.push_back(sc::pick(ctx, viewOfXy(), 50, 50, near(5)).status);
    };
    Obj command;
    command.methods["canRun"] = [&](void **, void *ret, string *) {
      all(inCanRun);
      new (ret) Status();
      return true;
    };
    command.methods["exec"] = [&](void **, void *ret, string *) {
      all(inExec);
      new (ret) Status();
      return true;
    };
    command.methods["undo"] = [](void **, void *, string *) { return true; };
    command.methods["discard"] = [](void **, void *, string *) { return true; };
    command.methods["memSize"] = [](void **, void *ret, string *) {
      *static_cast<uint64_t *>(ret) = 0;
      return true;
    };
    Obj factory;
    factory.methods["make"] = [&](void **, void *ret, string *) {
      *static_cast<uint32_t *>(ret) = host.add(command);
      return true;
    };
    CommandDef *def = sc::newCommandDef("js.cmd");
    must(sc::registerForeignCommand(
        ctx, *def, host.add(factory), "canRun exec undo memSize discard"));
    litestl::alloc::Delete(def);
    const std::string before = CommandDoc::liveOnly(d.describe());
    const sc::BagResult ran = sc::invoke(ctx, "js.cmd");
    if (!ran.status.ok()) {
      fprintf(stderr, "%s\n", ran.status.message());
    }
    test_assert(ran.status.ok() && inCanRun.size() >= 6 && inExec.size() == 6);
    /* Reads (blockIdText, worldMatrix) are allowed in every slot; the rest are Busy. */
    for (size_t i = 0; i < inCanRun.size(); i++) {
      const bool read = i % 6 == 2 || i % 6 == 4;
      test_assert(read ? inCanRun[i].ok() : inCanRun[i].code() == StatusCode::Busy);
    }
    for (const Status &s : inExec) {
      if (!s.ok()) {
        fprintf(stderr, "in exec: %s\n", s.message());
      }
      test_assert(s.ok());
    }
    /* The scene is undone with the command. */
    test_assert(CommandDoc::liveOnly(d.describe()) != before);
    must(sc::undo(ctx));
    test_assert(CommandDoc::liveOnly(d.describe()) == before);
  }
  test_assert(host.allReleasedOnce());
}

void testFacades()
{
  CommandDoc d;
  Context &ctx = *d.ctx;
  const BlockHandle first = d.scene;
  const BlockHandle a = d.object("a", 1), gone = d.object("gone");
  const std::string goneText = textOf(d, gone);
  must(d.lib->markDeleted(gone));

  const sc::ValueResult made = sc::newScene(ctx, "Second");
  test_assert(made.status.ok() && ctx.scene() == first && d.history->size() == 1);
  const BlockHandle second = d.lib->find(made.value.tryPointer()->owner.block);
  test_assert(Scene::of(*d.lib, second) && isText(d.lib->get(second)->name(), "Second"));
  must(sc::setScene(ctx, str(textOf(d, second))));
  test_assert(ctx.scene() == second);

  struct Bad {
    std::string text;
    StatusCode code;
  };
  const Bad bad[] = {
      {"nonsense", StatusCode::InvalidArgument},
      {"@owner(root)", StatusCode::InvalidArgument},
      {"@block(coredcc, 0123456789abcdef0123456789abcdef)", StatusCode::NotFound},
      {goneText, StatusCode::NotFound},
  };
  for (const Bad &b : bad) {
    test_assert(isError(sc::setScene(ctx, str(b.text)), b.code, "setScene: "));
    test_assert(
        isError(sc::worldMatrix(ctx, str(b.text)).status, b.code, "worldMatrix: "));
    test_assert(
        isError(sc::blockIdText(ctx, str(b.text)).status, b.code, "blockIdText: "));
  }
  test_assert(isError(sc::worldMatrix(ctx, "").status, StatusCode::InvalidArgument));
  test_assert(isError(sc::blockIdText(ctx, "").status, StatusCode::InvalidArgument));
  test_assert(isError(
      sc::setScene(ctx, str(textOf(d, a))), StatusCode::InvalidArgument, "scene"));
  std::string member = textOf(d, a);
  member.insert(member.find(')') + 1, ".x");
  test_assert(
      isError(sc::worldMatrix(ctx, str(member)).status, StatusCode::InvalidArgument));
  test_assert(ctx.scene() == second);
  test_assert(isError(sc::worldMatrix(ctx, str(textOf(d, second))).status,
                      StatusCode::FailedPrecondition));
  test_assert(isText(sc::blockIdText(ctx, str(textOf(d, a))).value,
                     formatBlockId(d.rt, d.idOf(a))));

  /* Not a member of the scene, and without any evaluation. */
  sc::MatrixResult m = sc::worldMatrix(ctx, str(textOf(d, a)));
  test_assert(m.status.ok() && solve::translationOf(m.value)[0] == 1);
  put(d.obj(a).channels, trs::kLocation, double3(8, 0, 0));
  test_assert(solve::translationOf(sc::worldMatrix(ctx, str(textOf(d, a))).value)[0] ==
              8);

  must(sc::setScene(ctx, ""));
  test_assert(!ctx.scene());
  test_assert(isError(sc::evaluateScene(ctx), StatusCode::NotFound));
  test_assert(isError(sc::pick(ctx, viewOfXy(), 50, 50, near(5)).status,
                      StatusCode::FailedPrecondition,
                      "no scene"));
  must(sc::setScene(ctx, str(textOf(d, first))));

  Context none(&d.rt, nullptr, PointerRef());
  test_assert(isError(sc::setScene(none, str(textOf(d, first))), StatusCode::NotFound));
  test_assert(isError(
      sc::newScene(none, "x").status, StatusCode::FailedPrecondition, "executor"));
  test_assert(
      isError(sc::worldMatrix(none, str(textOf(d, a))).status, StatusCode::NotFound));
  test_assert(isError(sc::evaluateScene(none), StatusCode::FailedPrecondition));

  double3 v = sc::vec3(1, 2, 3);
  sc::vec3Put(v, 1, 5);
  sc::vec3Put(v, 3, 9);
  sc::vec3Put(v, -1, 9);
  test_assert(sc::vec3At(v, 0) == 1 && sc::vec3At(v, 1) == 5 && sc::vec3At(v, 2) == 3);
  test_assert(sc::vec3At(v, 3) == 0 && sc::vec3At(v, -1) == 0);
}

void testPick()
{
  fakehost::Host host;
  Polar model;
  {
    CommandDoc d;
    Context &ctx = *d.ctx;
    const BlockHandle shape = d.shapeBlock("shape", 1.0);
    const BlockHandle low = d.object("low", 3, 0, -2), high = d.object("high", 3, 0, 2),
                      hidden = d.object("hidden", 3), off = d.object("off", -5);
    for (BlockHandle h : {low, high, hidden, off}) {
      d.setData(h, shape);
    }
    d.obj(hidden).flags = ObjectFlags::Hidden;
    /* Pixel (65, 50) is (3, 0); no evaluation was made. */
    sc::PickResult r = sc::pick(ctx, viewOfXy(), 65, 50, near(2));
    test_assert(r.status.ok() && r.value.size() == 2);
    test_assert(isText(r.value.at(0).object, textOf(d, high)));
    test_assert(isText(r.value.at(1).object, textOf(d, low)));
    test_assert(r.value.at(0).distance == 0 &&
                r.value.at(0).viewDistance < r.value.at(1).viewDistance);
    test_assert(std::abs(sc::vec3At(r.value.at(0).point, 0) - 3) < 1e-9);
    test_assert(r.value.at(2).object.size() == 0 && r.value.at(-1).domain == -1);
    /* It evaluates. */
    put(d.obj(high).channels, trs::kLocation, double3(-5, 0, 0));
    r = sc::pick(ctx, viewOfXy(), 65, 50, near(2));
    test_assert(r.status.ok() && r.value.size() == 1);
    test_assert(isText(r.value.at(0).object, textOf(d, low)));

    View bad = viewOfXy();
    bad.viewport[2] = 0;
    test_assert(sc::pick(ctx, bad, 65, 50, near(2)).status.code() ==
                StatusCode::InvalidArgument);

    /* A model that throws doesn't keep the others from being found. */
    must(registerPolar(d, host, model, "js.polar"));
    d.obj(off).setTransformType("js.polar");
    d.obj(off).channels = d.rt.ext<TransformRegistry>().find("js.polar")->defaults();
    model.throwEvaluate = true;
    r = sc::pick(ctx, viewOfXy(), 65, 50, near(2));
    test_assert(isError(r.status, StatusCode::Internal, "no state") &&
                !d.rt.hasStickyError());
    test_assert(r.value.size() == 1 && isText(r.value.at(0).object, textOf(d, low)));
    model.throwEvaluate = false;

    /* An evaluation that fails gives no old results. */
    const BlockHandle host2 = d.hostObject("host", 0);
    must(sc::evaluateScene(ctx));
    int flip = 0;
    d.host(host2).onRead = [&](ObjectInfo &info) {
      info.parent = d.idOf(flip++ % 2 ? low : high);
    };
    r = sc::pick(ctx, viewOfXy(), 65, 50, near(2));
    test_assert(isError(r.status, StatusCode::FailedPrecondition) && r.value.size() == 0);
    d.host(host2).onRead = nullptr;
    d.rt.setStickyError(Status::error(StatusCode::Internal, "earlier"));
    r = sc::pick(ctx, viewOfXy(), 65, 50, near(2));
    test_assert(isError(r.status, StatusCode::FailedPrecondition) && r.value.size() == 0);
    test_assert(!strcmp(d.rt.takeStickyError().message(), "earlier"));
  }
  test_assert(host.allReleasedOnce());
}

/* What is bound refuses a NUL in any string. */
void testNul()
{
  using sc::detail::Checked;
  CommandDoc d;
  string nul("a");
  nul += char(0);
  nul += "b";
  Context &ctx = *d.ctx;
  const StatusCode bad = StatusCode::InvalidArgument;
  const int scenes = d.lib->size();
  test_assert(isError(Checked<&sc::newScene>::call(ctx, nul).status, bad, "argument 2"));
  test_assert(isError(Checked<&sc::setScene>::call(ctx, nul), bad, "argument 2"));
  test_assert(
      isError(Checked<&sc::worldMatrix>::call(ctx, nul).status, bad, "argument 2"));
  test_assert(
      isError(Checked<&sc::blockIdText>::call(ctx, nul).status, bad, "argument 2"));
  test_assert(
      isError(Checked<&sc::formatValue>::call(ctx, nul).status, bad, "argument 2"));
  test_assert(isError(
      Checked<&sc::parseValue>::call(ctx, "scene", nul).status, bad, "argument 3"));
  test_assert(d.lib->size() == scenes);
  TransformTypeInfo *info = sc::newTransformTypeDef("js.nul");
  test_assert(
      isError(Checked<&sc::transformTypeDefUiName>::call(*info, nul), bad, "argument 2"));
  test_assert(isError(
      Checked<&sc::transformTypeDefChannel>::call(*info, nul, sc::makeFloat(1), false),
      bad,
      "argument 2"));
  test_assert(info->channels.size() == 0);
  {
    fakehost::Host host;
    const fakehost::lf::Handle h = host.add(Obj());
    using Entry = Checked<&sc::registerForeignTransformType, 3>;
    test_assert(isError(Entry::call(ctx, *info, h, nul), bad, "argument 4"));
    test_assert(host.releasesOf(h) == 1);
  }
  litestl::alloc::Delete(info);
  /* A def's id is refused later. */
  info = sc::newTransformTypeDef(nul);
  test_assert(isError(d.rt.ext<TransformRegistry>().checkInfo(*info), bad, "NUL"));
  litestl::alloc::Delete(info);
}

} // namespace

int main()
{
  test_assert(sc::bindings());
  testRegistration();
  testModel();
  testGates();
  testCommandSlots();
  testFacades();
  testNul();
  testPick();
  return test_end();
}

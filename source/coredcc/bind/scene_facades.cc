#include "coredcc/bind/scene_facades.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/bind/detail.h"
#include "coredcc/command/executor.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/pick_scene.h"

#include <string>

namespace coredcc::script {

using detail::error;
using detail::failed;
using detail::ok;
using detail::slotGate;
using detail::view;
namespace lbt = litestl::binding::types;

double3 vec3(double x, double y, double z)
{
  return double3(x, y, z);
}

double vec3At(const double3 &v, int32_t i)
{
  return i >= 0 && i < 3 ? v[i] : 0.0;
}

void vec3Put(double3 &v, int32_t i, double x)
{
  if (i >= 0 && i < 3) {
    v[i] = x;
  }
}

/* The Live block that a pointer's text names. */
static Result<BlockHandle> blockAt(Context &ctx, const char *what, const string &text)
{
  Result<StableRef> ref = parseRef(ctx.runtime(), view(text));
  if (!ref.ok()) {
    return error(ref.status().code(), std::string(what) + ": " + ref.status().message());
  }
  if (ref->owner.kind != OwnerKey::Kind::Block || ref->key.size() > 0) {
    return error(StatusCode::InvalidArgument,
                 std::string(what) + ": '" + text.c_str() + "' isn't a block");
  }
  const Library *lib = ctx.library();
  const BlockHandle h = lib ? lib->find(ref->owner.block) : BlockHandle();
  const Datablock *b = lib ? lib->get(h) : nullptr;
  if (!b || b->state() != BlockState::Live) {
    return error(StatusCode::NotFound,
                 std::string(what) + ": there is no block " + text.c_str());
  }
  return h;
}

ValueResult newScene(Context &ctx, const string &name)
{
  if (Status s = slotGate(ctx, "newScene", {"exec"}); !s.ok()) {
    return failed<ValueResult>(std::move(s));
  }
  if (!ctx.executor()) {
    return failed<ValueResult>(
        error(StatusCode::FailedPrecondition, "newScene: the context has no executor"));
  }
  const CommandRegistry &reg = ctx.runtime().ext<CommandRegistry>();
  const CommandDef *def = reg.find(kNewSceneCommand);
  if (!def) {
    return failed<ValueResult>(reg.unknown(kNewSceneCommand));
  }
  PropertyBag in = def->newInputs();
  if (Status s = in.set("name", PropValue::ofString(name.c_str())); !s.ok()) {
    return failed<ValueResult>(std::move(s));
  }
  Result<PropertyBag> out = detail::runCommand(ctx, *def, std::move(in));
  return out.ok() ? ok<ValueResult>(*out->get("scene")) : failed<ValueResult>(out.status());
}

Status setScene(Context &ctx, const string &scene)
{
  CDCC_TRY(slotGate(ctx, "setScene", {"exec"}));
  if (scene.size() == 0) {
    ctx.setScene(BlockHandle());
    return Status();
  }
  Result<BlockHandle> h = blockAt(ctx, "setScene", scene);
  if (!h.ok()) {
    return h.status();
  }
  if (!Scene::of(*ctx.library(), *h)) {
    return error(StatusCode::InvalidArgument,
                 std::string("setScene: '") + scene.c_str() + "' isn't a scene");
  }
  ctx.setScene(*h);
  return Status();
}

StringResult blockIdText(Context &ctx, const string &block)
{
  if (Status s = slotGate(ctx, "blockIdText", {"exec"}); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<BlockHandle> h = blockAt(ctx, "blockIdText", block);
  if (!h.ok()) {
    return failed<StringResult>(h.status());
  }
  const BlockId id = ctx.library()->get(*h)->id();
  return ok<StringResult>(string(formatBlockId(ctx.runtime(), id).c_str()));
}

Status evaluateScene(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "evaluateScene", {"exec"}));
  return coredcc::evaluateScene(ctx);
}

MatrixResult worldMatrix(Context &ctx, const string &object)
{
  if (Status s = slotGate(ctx, "worldMatrix", {"exec"}); !s.ok()) {
    return failed<MatrixResult>(std::move(s));
  }
  Result<BlockHandle> h = blockAt(ctx, "worldMatrix", object);
  if (!h.ok()) {
    return failed<MatrixResult>(h.status());
  }
  const Scene *scene = activeScene(ctx);
  Result<TransformState> s = computeState(*ctx.library(), *h, scene ? scene->time() : 0.0);
  return s.ok() ? ok<MatrixResult>(s->world) : failed<MatrixResult>(s.status());
}

PickResult pick(Context &ctx, const View &view, double x, double y, const PickOptions &options)
{
  if (Status s = slotGate(ctx, "pick", {"exec"}); !s.ok()) {
    return failed<PickResult>(std::move(s));
  }
  const Scene *scene = activeScene(ctx);
  if (!scene) {
    return failed<PickResult>(error(StatusCode::FailedPrecondition, "pick: there is no scene"));
  }
  ViewMath math;
  if (Status s = math.set(view); !s.ok()) {
    return failed<PickResult>(std::move(s));
  }
  PickResult r;
  const int before = scene->counters().completed;
  r.status = coredcc::evaluateScene(ctx);
  if (!r.status.ok() && scene->counters().completed == before) {
    return r;
  }
  litestl::util::Vector<PickHit> hits;
  Status picked = pickScene(ctx, ctx.scene(), math, x, y, options, PickFilter(), hits);
  if (r.status.ok()) {
    r.status = std::move(picked);
  }
  for (const PickHit &h : hits) {
    const PropValue pointer = coredcc::detail::pointerTo(*ctx.library(), h.object);
    PickedObject p;
    p.object = string(formatRef(ctx.runtime(), *pointer.tryPointer()).c_str());
    p.domain = h.domain;
    p.element = h.element;
    p.distance = h.distance;
    p.depth = h.depth;
    p.point = h.point;
    p.instance = h.instance;
    p.normal = h.normal;
    p.u = h.u;
    p.v = h.v;
    p.backFacing = h.backFacing;
    p.thin = h.thin;
    r.value.hits.append(std::move(p));
  }
  return r;
}

const lbt::Struct<PickedObject> *PickedObject::defineBindings()
{
  static lbt::Struct<PickedObject> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PickedObject>("coredcc::PickedObject", sizeof(PickedObject));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, object);
    BIND_STRUCT_MEMBER(st, domain);
    BIND_STRUCT_MEMBER(st, element);
    BIND_STRUCT_MEMBER(st, distance);
    BIND_STRUCT_MEMBER(st, depth);
    BIND_STRUCT_MEMBER(st, point);
    BIND_STRUCT_MEMBER(st, instance);
    BIND_STRUCT_MEMBER(st, normal);
    BIND_STRUCT_MEMBER(st, u);
    BIND_STRUCT_MEMBER(st, v);
    BIND_STRUCT_MEMBER(st, backFacing);
    BIND_STRUCT_MEMBER(st, thin);
  }
  return st;
}

const lbt::Struct<PickList> *PickList::defineBindings()
{
  static lbt::Struct<PickList> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PickList>("coredcc::PickList", sizeof(PickList));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_STRUCT_METHOD(st, at, MARGS("i"));
  }
  return st;
}

CDCC_RESULT_BINDINGS(MatrixResult)
CDCC_RESULT_BINDINGS(PickResult)

} // namespace coredcc::script

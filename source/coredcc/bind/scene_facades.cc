#include "coredcc/bind/scene_facades.h"

#include "coredcc/api/change_bus.h"
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
using litestl::util::Vector;
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
  return out.ok() ? ok<ValueResult>(*out->get("scene"))
                  : failed<ValueResult>(out.status());
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

/* The Context's scene, and the places: all of them, or those that were given. */
static Result<const Scene *> placesOf(Context &ctx,
                                      const char *what,
                                      const Vector<int32_t> &given,
                                      bool all,
                                      Vector<int32_t> &out)
{
  const Scene *scene = activeScene(ctx);
  if (!scene) {
    return error(StatusCode::FailedPrecondition,
                 std::string(what) + ": there is no scene");
  }
  const int32_t n = int32_t(scene->results().size());
  for (int32_t i = 0; all && i < n; i++) {
    out.append(i);
  }
  for (int32_t i : given) {
    if (all || i < 0 || i >= n) {
      return error(all ? StatusCode::InvalidArgument : StatusCode::OutOfRange,
                   std::string(what) +
                       (all ? ": both places and all" : ": no such place"));
    }
    out.append(i);
  }
  return scene;
}

SceneTableResult sceneResults(Context &ctx, const Vector<int32_t> &places, bool all)
{
  if (Status s = slotGate(ctx, "sceneResults", {"exec"}); !s.ok()) {
    return failed<SceneTableResult>(std::move(s));
  }
  Vector<int32_t> at;
  Result<const Scene *> scene = placesOf(ctx, "sceneResults", places, all, at);
  if (!scene.ok()) {
    return failed<SceneTableResult>(scene.status());
  }
  SceneTableResult r;
  SceneTable &t = r.value;
  t.matrixList.ensure_capacity(at.size() * 16);
  t.boundsList.ensure_capacity(at.size() * 6);
  for (int32_t i : at) {
    const ObjectEval &e = (*scene)->results()[size_t(i)];
    const double *m = e.dataWorld();
    for (int k = 0; k < 16; k++) {
      t.matrixList.append(m[k]);
    }
    for (int k = 0; k < 6; k++) {
      t.boundsList.append(k < 3 ? e.bounds.min[k] : e.bounds.max[k - 3]);
    }
    t.statusList.append(int32_t(e.status));
    t.flagsList.append(int32_t(e.flags));
    t.parentList.append(e.parent);
    t.memberList.append(e.member ? 1 : 0);
  }
  const Scene::Changes &changes = (*scene)->lastChanges();
  t.changedList = changes.places;
  t.changeVersion = double(changes.version);
  t.changedAll = changes.all;
  t.placeCount = int32_t((*scene)->results().size());
  return r;
}

static string refText(const Context &ctx, BlockHandle h)
{
  const Library &lib = *ctx.library();
  if (!lib.get(h)) {
    return string();
  }
  const PropValue pointer = coredcc::detail::pointerTo(lib, h);
  return string(formatRef(ctx.runtime(), *pointer.tryPointer()).c_str());
}

RefListResult sceneRefs(Context &ctx, const Vector<int32_t> &places, bool all)
{
  if (Status s = slotGate(ctx, "sceneRefs", {"exec"}); !s.ok()) {
    return failed<RefListResult>(std::move(s));
  }
  Vector<int32_t> at;
  Result<const Scene *> scene = placesOf(ctx, "sceneRefs", places, all, at);
  if (!scene.ok()) {
    return failed<RefListResult>(scene.status());
  }
  RefListResult r;
  for (int32_t i : at) {
    r.value.refs.append(refText(ctx, (*scene)->evaluatedObjects()[i]));
  }
  return r;
}

RefListResult sceneChildren(Context &ctx, const string &object)
{
  if (Status s = slotGate(ctx, "sceneChildren", {"exec"}); !s.ok()) {
    return failed<RefListResult>(std::move(s));
  }
  const Scene *scene = activeScene(ctx);
  if (!scene) {
    return failed<RefListResult>(
        error(StatusCode::FailedPrecondition, "sceneChildren: there is no scene"));
  }
  BlockHandle h;
  if (object.size() > 0) {
    Result<BlockHandle> at = blockAt(ctx, "sceneChildren", object);
    if (!at.ok()) {
      return failed<RefListResult>(at.status());
    }
    h = *at;
  }
  RefListResult r;
  for (BlockHandle child : scene->children(h)) {
    r.value.refs.append(refText(ctx, child));
  }
  return r;
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
  Result<TransformState> s =
      computeState(*ctx.library(), *h, scene ? scene->time() : 0.0);
  return s.ok() ? ok<MatrixResult>(s->world) : failed<MatrixResult>(s.status());
}

Status notifyChanged(Context &ctx, const string &block)
{
  CDCC_TRY(slotGate(ctx, "notifyChanged", {"exec"}));
  Result<BlockHandle> h = blockAt(ctx, "notifyChanged", block);
  CDCC_TRY(h.status());
  ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(ctx.library()->get(*h)->id()));
  return Status();
}

PickResult
pick(Context &ctx, const View &view, double x, double y, const PickOptions &options)
{
  if (Status s = slotGate(ctx, "pick", {"exec"}); !s.ok()) {
    return failed<PickResult>(std::move(s));
  }
  const Scene *scene = activeScene(ctx);
  if (!scene) {
    return failed<PickResult>(
        error(StatusCode::FailedPrecondition, "pick: there is no scene"));
  }
  ViewMath math;
  if (Status s = math.set(view); !s.ok()) {
    return failed<PickResult>(std::move(s));
  }
  PickResult r;
  EvaluateResult evaluated;
  r.status = coredcc::evaluateScene(ctx, &evaluated);
  if (!evaluated.done) {
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

const lbt::Struct<SceneTable> *SceneTable::defineBindings()
{
  static lbt::Struct<SceneTable> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<SceneTable>("coredcc::SceneTable", sizeof(SceneTable));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, matrices, MARGS());
    BIND_STRUCT_METHOD(st, bounds, MARGS());
    BIND_STRUCT_METHOD(st, status, MARGS());
    BIND_STRUCT_METHOD(st, flags, MARGS());
    BIND_STRUCT_METHOD(st, parents, MARGS());
    BIND_STRUCT_METHOD(st, members, MARGS());
    BIND_STRUCT_METHOD(st, changed, MARGS());
    BIND_STRUCT_METHOD(st, version, MARGS());
    BIND_STRUCT_METHOD(st, allChanged, MARGS());
    BIND_STRUCT_METHOD(st, places, MARGS());
  }
  return st;
}

const lbt::Struct<RefList> *RefList::defineBindings()
{
  static lbt::Struct<RefList> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<RefList>("coredcc::RefList", sizeof(RefList));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_STRUCT_METHOD(st, at, MARGS("i"));
  }
  return st;
}

CDCC_RESULT_BINDINGS(SceneTableResult)
CDCC_RESULT_BINDINGS(RefListResult)
CDCC_RESULT_BINDINGS(MatrixResult)
CDCC_RESULT_BINDINGS(PickResult)

} // namespace coredcc::script

#include "coredcc/scene/evaluated.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/scene/scene.h"

#include "litestl/util/alloc.h"

#include <cstring>

namespace coredcc {

using litestl::util::Vector;

bool ObjectEval::same(const ObjectEval &b) const
{
  const auto equal = [](const auto &x, const auto &y) {
    static_assert(sizeof(x) % sizeof(double) == 0);
    return memcmp(&x, &y, sizeof(x)) == 0;
  };
  return status == b.status && member == b.member && equal(state.world, b.state.world) &&
         equal(state.rotation, b.state.rotation) && equal(state.localScale, b.state.localScale) &&
         equal(localBounds, b.localBounds) && equal(bounds, b.bounds);
}

/* ---- the owner ---- */

namespace detail {

bool HandleIndex::same(const HandleIndex &b) const
{
  bool same = handles_.size() == b.handles_.size();
  for (int i = 0; same && i < int(handles_.size()); i++) {
    same = handles_[i] == b.handles_[i];
  }
  return same;
}

namespace {

constexpr const char *kEvaluatedStruct = "coredcc.EvaluatedScene";
constexpr const char *kEvaluatedObjectStruct = "coredcc.EvaluatedObject";

/* What the owner's structs are instances of: an id per object that was asked for. The results
 * themselves move with every evaluation. */
class EvaluatedKeys {
 public:
  static constexpr const char *kExtName = "coredcc.EvaluatedKeys";
  OwnerKey owner;

  explicit EvaluatedKeys(Runtime *) {}
  ~EvaluatedKeys()
  {
    for (BlockId *id : ids_) {
      litestl::alloc::Delete(id);
    }
  }
  BlockId *keyFor(const BlockId &id)
  {
    if (BlockId *const *known = byId_.lookup_ptr(id)) {
      return *known;
    }
    BlockId *made = litestl::alloc::New<BlockId>("coredcc::EvaluatedKeys", id);
    ids_.append(made);
    byId_.add(id, made);
    return made;
  }

 private:
  Vector<BlockId *> ids_;
  litestl::util::Map<BlockId, BlockId *> byId_;
};

bool evalOf(Context &ctx, const BlockId &id, ObjectEval &out)
{
  const Scene *scene = activeScene(ctx);
  return scene && scene->evaluated(ctx.library()->find(id), out);
}

PointerRef itemFor(Context &ctx, const BlockId &id)
{
  ObjectEval e;
  return evalOf(ctx, id, e) ? PointerRef{ctx.runtime().ext<EvaluatedKeys>().keyFor(id),
                                         StructId::of(kEvaluatedObjectStruct),
                                         OwnerKey()} :
                              PointerRef();
}

/* The results that still have a record. */
Vector<BlockHandle> handles(Context &ctx)
{
  Vector<BlockHandle> out;
  if (const Scene *scene = activeScene(ctx)) {
    for (BlockHandle h : scene->evaluatedObjects()) {
      if (ctx.library()->get(h)) {
        out.append(h);
      }
    }
  }
  return out;
}

ApiStruct sceneStruct()
{
  ApiStruct st(kEvaluatedStruct);
  st.uiName("Evaluated Scene").description("The results of the last evaluation.");
  CollectionAccessor c;
  c.itemStruct = StructId::of(kEvaluatedObjectStruct);
  c.length = [](const PointerRef &, Context &ctx) { return int64_t(handles(ctx).size()); };
  c.at = [](const PointerRef &, Context &ctx, int64_t i) {
    const Vector<BlockHandle> l = handles(ctx);
    return i >= 0 && i < int64_t(l.size()) ?
               itemFor(ctx, ctx.library()->get(l[int(i)])->id()) :
               PointerRef();
  };
  c.lookup = [](const PointerRef &, Context &ctx, const KeySegment &key) {
    if (key.kind != KeySegment::Kind::StringKey) {
      return PointerRef();
    }
    const Result<BlockId> id = parseBlockId(ctx.runtime(), key.text.c_str());
    return id.ok() && !id->isNull() ? itemFor(ctx, *id) : PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    return KeySegment::stringKey(
        formatBlockId(ctx.runtime(), *static_cast<const BlockId *>(item.data)));
  };
  st.addCollection(PropertyDef::collectionProp("objects", c.itemStruct), std::move(c));
  return st;
}

PropValue corner(const Box3d &b, bool max)
{
  return max ? PropValue::ofVec3(b.max[0], b.max[1], b.max[2]) :
               PropValue::ofVec3(b.min[0], b.min[1], b.min[2]);
}

ApiStruct resultStruct()
{
  ApiStruct st(kEvaluatedObjectStruct);
  st.uiName("Evaluated Object");
  const auto add = [&st](PropertyDef def, PropValue (*read)(const ObjectEval &)) {
    ValueAccessor a;
    a.get = [read](const PointerRef &p, Context &ctx) {
      ObjectEval e;
      evalOf(ctx, *static_cast<const BlockId *>(p.data), e);
      return read(e);
    };
    st.add(std::move(def), std::move(a));
  };
  add(PropertyDef::mat4Prop("world"), [](const ObjectEval &e) {
    return PropValue::ofComponents(PropValue::Type::Mat4,
                                   static_cast<const double *>(e.state.world));
  });
  add(PropertyDef::vecProp("boundsMin", 3), [](const ObjectEval &e) { return corner(e.bounds, false); });
  add(PropertyDef::vecProp("boundsMax", 3), [](const ObjectEval &e) { return corner(e.bounds, true); });
  add(PropertyDef::enumProp("status")
          .item("OK", int64_t(EvalStatus::Ok))
          .item("INVALID", int64_t(EvalStatus::Invalid))
          .item("MODEL_FAILED", int64_t(EvalStatus::ModelFailed))
          .item("MISSING_MODEL", int64_t(EvalStatus::MissingModel))
          .item("CYCLE", int64_t(EvalStatus::Cycle))
          .item("NO_TRANSFORM", int64_t(EvalStatus::NoTransform)),
      [](const ObjectEval &e) { return PropValue::ofEnum(int64_t(e.status)); });
  return st;
}

} // namespace

OwnerKey evaluatedOwner(Runtime &rt)
{
  return rt.ext<EvaluatedKeys>().owner;
}

Status registerEvaluated(Runtime &rt)
{
  ApiRegistry &api = rt.ext<ApiRegistry>();
  CDCC_TRY(api.add(resultStruct()).status());
  CDCC_TRY(api.add(sceneStruct()).status());
  EvaluatedKeys *keys = &rt.ext<EvaluatedKeys>();
  Result<OwnerKey> owner = api.addRuntimeOwner(
      kEvaluatedOwner, StructId::of(kEvaluatedStruct), [keys]() -> void * { return keys; });
  if (!owner.ok()) {
    return owner.status();
  }
  keys->owner = *owner;
  return Status();
}

bool evaluatedRegistered(Runtime &rt)
{
  return !rt.ext<EvaluatedKeys>().owner.isNull();
}

} // namespace detail

void addEvaluated(ApiStruct &root)
{
  PointerAccessor a;
  a.target = StructId::of(detail::kEvaluatedStruct);
  a.get = [](const PointerRef &, Context &ctx) {
    return ctx.runtime().ext<ApiRegistry>().runtimeOwner(kEvaluatedOwner);
  };
  root.addPointer(PropertyDef::pointerProp("evaluated", a.target), std::move(a));
}

} // namespace coredcc

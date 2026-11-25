#include "coredcc/scene/evaluated.h"

#include "coredcc/scene/scene.h"

#include "litestl/util/alloc.h"

#include <cstdio>
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

/* ---- block ids as text ---- */

std::string blockIdText(const Runtime &rt, const BlockId &id)
{
  char hex[40];
  snprintf(hex,
           sizeof(hex),
           "%016llx%016llx",
           static_cast<unsigned long long>(id.local.hi),
           static_cast<unsigned long long>(id.local.lo));
  std::string client;
  if (const char *name = rt.nameOf(id.client)) {
    client = name;
  }
  else {
    char hash[20];
    snprintf(hash, sizeof(hash), "#%016llx", static_cast<unsigned long long>(id.client.value));
    client = hash;
  }
  return client + "," + hex;
}

namespace {

bool parseHex(std::string_view s, uint64_t &out)
{
  if (s.size() != 16) {
    return false;
  }
  out = 0;
  for (char c : s) {
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) {
      return false;
    }
    out = out << 4 | uint64_t(d);
  }
  return true;
}

} // namespace

BlockId parseBlockIdText(std::string_view text)
{
  const size_t comma = text.rfind(',');
  BlockId id;
  if (comma == std::string_view::npos || comma == 0 || text.size() - comma - 1 != 32 ||
      !parseHex(text.substr(comma + 1, 16), id.local.hi) ||
      !parseHex(text.substr(comma + 17, 16), id.local.lo))
  {
    return BlockId();
  }
  const std::string_view client = text.substr(0, comma);
  if (client[0] != '#') {
    id.client = ClientId::of(client);
  }
  else if (!parseHex(client.substr(1), id.client.value)) {
    return BlockId();
  }
  return id;
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
    const BlockId id = key.kind == KeySegment::Kind::StringKey ?
                           parseBlockIdText(key.text.c_str()) :
                           BlockId();
    return id.isNull() ? PointerRef() : itemFor(ctx, id);
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    return KeySegment::stringKey(
        blockIdText(ctx.runtime(), *static_cast<const BlockId *>(item.data)));
  };
  st.addCollection(PropertyDef::collectionProp("objects", c.itemStruct), std::move(c));
  return st;
}

PropValue corner(const Box3d &b, bool max)
{
  return max ? PropValue::ofVec3(b.max[0], b.max[1], b.max[2]) :
               PropValue::ofVec3(b.min[0], b.min[1], b.min[2]);
}

ApiStruct objectStruct()
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
  CDCC_TRY(api.add(objectStruct()).status());
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

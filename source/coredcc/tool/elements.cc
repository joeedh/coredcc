#include "coredcc/tool/elements.h"

#include "coredcc/base/invalidate.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/object.h"
#include "coredcc/scene/scene.h"

#include "litestl/binding/binding.h"
#include "litestl/math/transform.h"
#include "litestl/util/map.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

/* ---- ElementSet ---- */

void ElementSet::add(int32_t handle, double x, double y, double z, double weight)
{
  handles.append(handle);
  positions.append(x);
  positions.append(y);
  positions.append(z);
  weights.append(weight);
}

void ElementSet::clear()
{
  handles.clear();
  positions.clear();
  weights.clear();
}

lbt::Struct<ElementSet> *ElementSet::defineBindings()
{
  static lbt::Struct<ElementSet> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ElementSet>("coredcc::ElementSet", sizeof(ElementSet));
    BIND_STRUCT_METHOD(st, add, MARGS("handle", "x", "y", "z", "weight"));
    BIND_STRUCT_METHOD(st, size, MARGS());
  }
  return st;
}

/* ---- symmetry ---- */

namespace detail {

BlockHandle elementsBlock(const Library &lib, BlockHandle object)
{
  if (const Transformable t = lib.query<Transformable>(object)) {
    ObjectInfo info;
    ChannelBlock channels;
    t.read(info, channels);
    const BlockHandle data = lib.find(info.data);
    if (data && lib.query<TransformableElements>(data)) {
      return data;
    }
  }
  return lib.query<TransformableElements>(object) ? object : BlockHandle();
}

Status findPartners(const Symmetry &sym,
                    uint32_t axes,
                    const Vector<int32_t> &moved,
                    Vector<uint8_t> &onPlanes,
                    Vector<int32_t> &partners,
                    Vector<int32_t> &source,
                    Vector<uint8_t> &across)
{
  const int n = int(moved.size());
  onPlanes.clear();
  partners.clear();
  source.clear();
  across.clear();
  for (int i = 0; i < n; i++) {
    onPlanes.append(0);
  }
  litestl::util::Map<int64_t, int32_t> isMoved;
  for (int i = 0; i < n; i++) {
    isMoved.add(moved[i], i);
  }
  /* The sources by handle, so that a tie goes to the lower one. */
  Vector<int32_t> order;
  for (int i = 0; i < n; i++) {
    order.append(i);
  }
  std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
    return moved[a] < moved[b];
  });
  /* Points on a mirror plane: their own partner across it. */
  for (int axis = 0; axis < 3; axis++) {
    if (!(axes & (1u << axis))) {
      continue;
    }
    const Vector<int32_t> p = sym.partners(moved, axis);
    if (int(p.size()) != n) {
      return Status::error(StatusCode::InvalidArgument,
                           "Symmetry::partners gave another number of handles");
    }
    for (int i = 0; i < n; i++) {
      onPlanes[i] |= p[i] == moved[i] ? uint8_t(1u << axis) : uint8_t(0);
    }
  }
  /* Sets of axes: fewest first, then the lower bits. */
  Vector<uint32_t> sets;
  for (uint32_t s = 1; s < 8; s++) {
    if ((s & axes) == s) {
      sets.append(s);
    }
  }
  std::stable_sort(sets.begin(), sets.end(), [](uint32_t a, uint32_t b) {
    const int ca = std::popcount(a), cb = std::popcount(b);
    return ca != cb ? ca < cb : a < b;
  });
  litestl::util::Map<int64_t, bool> taken;
  for (uint32_t s : sets) {
    /* Ask one axis after the other. */
    Vector<int32_t> cur = moved;
    for (int axis = 0; axis < 3; axis++) {
      if (!(s & (1u << axis))) {
        continue;
      }
      Vector<int32_t> ask;
      Vector<int32_t> at;
      for (int i = 0; i < n; i++) {
        if (cur[i] >= 0) {
          ask.append(cur[i]);
          at.append(i);
        }
      }
      const Vector<int32_t> got =
          ask.size() > 0 ? sym.partners(ask, axis) : Vector<int32_t>();
      if (got.size() != ask.size()) {
        return Status::error(StatusCode::InvalidArgument,
                             "Symmetry::partners gave another number of handles");
      }
      for (int i = 0; i < n; i++) {
        cur[i] = -1;
      }
      for (int k = 0; k < int(at.size()); k++) {
        cur[at[k]] = got[k];
      }
    }
    for (int32_t i : order) {
      const int32_t q = cur[i];
      if (q < 0 || isMoved.lookup_ptr(q) || taken.lookup_ptr(q)) {
        continue;
      }
      taken.add(q, true);
      partners.append(q);
      source.append(i);
      across.append(uint8_t(s));
    }
  }
  return Status();
}

} // namespace detail

/* ---- the command ---- */

namespace {

TransformDelta deltaFromInputs(Context &ctx)
{
  const PropertyBag &in = *ctx.inputs();
  const auto vec = [&in](const char *name) {
    const PropValue v = *in.get(name);
    return double3(v.component(0), v.component(1), v.component(2));
  };
  TransformDelta d;
  d.kind = DeltaKind(in.get("kind")->asEnum());
  const PropValue basis = *in.get("basis");
  for (int i = 0; i < 4; i++) {
    d.basis[i] = basis.component(i);
  }
  d.pivot = vec("pivot");
  d.vector = vec("vector");
  d.axis = vec("axis");
  d.angle = in.get("angle")->asFloat();
  return d;
}

TransformDelta weighted(const TransformDelta &d, double w)
{
  TransformDelta t = d;
  if (d.kind == DeltaKind::Translate) {
    t.vector = d.vector * w;
  } else if (d.kind == DeltaKind::Rotate) {
    t.angle = d.angle * w;
  } else {
    for (int i = 0; i < 3; i++) {
      t.vector[i] = 1.0 + w * (d.vector[i] - 1.0);
    }
  }
  return t;
}

Status notFound(const char *what)
{
  return Status::error(StatusCode::FailedPrecondition, what);
}

} // namespace

Status ElementsDelta::canRun(Context &ctx) const
{
  CDCC_TRY(detail::sceneOf(ctx).status());
  return Status();
}

Status ElementsDelta::exec(Context &ctx)
{
  const detail::SceneOf s = *detail::sceneOf(ctx);
  Result<Vector<BlockHandle>> objects = detail::targetObjects(ctx);
  CDCC_TRY(objects.status());
  const int32_t domain = int32_t(ctx.inputs()->get("domain")->asEnum());
  const bool symmetry = ctx.inputs()->get("symmetry")->asBool();
  const double time = s.scene->time();
  litestl::util::Map<uint64_t, bool>
      seen; /* objects that share a data block move it once */
  blocks_.clear();
  skipped_ = 0;
  for (BlockHandle h : *objects) {
    const BlockHandle data = detail::elementsBlock(*s.lib, h);
    const ElementAccess access =
        data ? s.lib->query<ElementAccess>(data) : ElementAccess();
    if (!data || !access || seen.lookup_ptr(data.computeHash())) {
      skipped_ += data && seen.lookup_ptr(data.computeHash()) ? 0 : 1;
      continue;
    }
    Result<TransformState> st = computeState(*s.lib, h, time);
    Block b;
    if (!st.ok() || !litestl::math::tryInverse(st->world, b.inverse)) {
      skipped_++;
      continue;
    }
    b.block = data;
    b.elements = s.lib->query<TransformableElements>(data);
    b.access = access;
    b.world = st->world;
    b.topoStamp = access.topoStamp();
    ElementSet set;
    CDCC_TRY(b.elements.gather(domain, set));
    CDCC_TRY(ctx.runtime().takeStickyError());
    if (set.size() == 0) {
      skipped_++;
      continue;
    }
    seen.add(data.computeHash(), true);
    b.handles = set.handles;
    b.start = set.positions;
    b.weights = set.weights;
    const Symmetry sym = symmetry ? s.lib->query<Symmetry>(data) : Symmetry();
    const uint32_t axes = sym ? (sym.axes() & 7u) : 0u;
    if (axes != 0) {
      CDCC_TRY(detail::findPartners(
          sym, axes, b.handles, b.onPlanes, b.partners, b.partnerSource, b.partnerAxes));
    } else {
      for (int i = 0; i < int(b.handles.size()); i++) {
        b.onPlanes.append(0);
      }
    }
    b.written = b.handles;
    for (int32_t q : b.partners) {
      b.written.append(q);
    }
    b.before = b.start;
    if (b.partners.size() > 0) {
      const Vector<double> p =
          access.positions(b.partners, int32_t(ElementDomain::Point));
      if (int(p.size()) != 3 * int(b.partners.size())) {
        return Status::error(StatusCode::InvalidArgument,
                             "ElementAccess::positions gave another number of positions");
      }
      for (double v : p) {
        b.before.append(v);
      }
    }
    b.after = b.before;
    blocks_.append(std::move(b));
  }
  if (blocks_.size() == 0) {
    return notFound("no selected object has elements to transform in that domain");
  }
  Status done = update(ctx, deltaFromInputs(ctx));
  ctx.outputs()->set("skipped", PropValue::ofInt(skipped_)).ignore();
  ctx.outputs()->set("points", PropValue::ofInt(moved())).ignore();
  return done;
}

Status ElementsDelta::update(Context &ctx, const TransformDelta &delta)
{
  for (Block &b : blocks_) {
    const int n = int(b.handles.size());
    Vector<double> pos;
    pos.resize(int(b.written.size()) * 3);
    mat4d full = b.inverse * delta.matrix() * b.world;
    for (int i = 0; i < n; i++) {
      const double w = b.weights[i];
      const mat4d m = w == 1.0 ? full : b.inverse * weighted(delta, w).matrix() * b.world;
      const double3 p(b.start[3 * i], b.start[3 * i + 1], b.start[3 * i + 2]);
      double3 q = litestl::math::transformPoint(m, p);
      for (int axis = 0; axis < 3; axis++) {
        if (b.onPlanes[i] & (1u << axis)) {
          q[axis] = 0.0; /* it stays on the mirror plane */
        }
      }
      for (int k = 0; k < 3; k++) {
        pos[3 * i + k] = q[k];
      }
    }
    for (int j = 0; j < int(b.partners.size()); j++) {
      const int src = b.partnerSource[j];
      for (int k = 0; k < 3; k++) {
        const double v = pos[3 * src + k];
        pos[3 * (n + j) + k] = (b.partnerAxes[j] & (1u << k)) ? -v : v;
      }
    }
    CDCC_TRY(write(ctx, b, pos));
    b.after = std::move(pos);
  }
  return Status();
}

Status ElementsDelta::write(Context &ctx, Block &b, const Vector<double> &positions)
{
  if (b.access.topoStamp() != b.topoStamp) {
    return Status::error(StatusCode::FailedPrecondition,
                         "the block's elements changed: its topoStamp is another");
  }
  CDCC_TRY(b.elements.setPositions(b.written, positions));
  CDCC_TRY(ctx.runtime().takeStickyError());
  return invalidateData(ctx, b.block, InvalidateKinds::Positions);
}

bool ElementsDelta::changed() const
{
  for (const Block &b : blocks_) {
    for (int i = 0; i < int(b.before.size()); i++) {
      if (b.before[i] != b.after[i]) {
        return true;
      }
    }
  }
  return false;
}

int32_t ElementsDelta::moved() const
{
  int32_t n = 0;
  for (const Block &b : blocks_) {
    n += int32_t(b.handles.size());
  }
  return n;
}

int32_t ElementsDelta::mirrored() const
{
  int32_t n = 0;
  for (const Block &b : blocks_) {
    n += int32_t(b.partners.size());
  }
  return n;
}

bool ElementsDelta::centre(double3 &out) const
{
  double3 sum(0, 0, 0);
  int n = 0;
  for (const Block &b : blocks_) {
    for (int i = 0; i < int(b.handles.size()); i++) {
      const double3 p(b.start[3 * i], b.start[3 * i + 1], b.start[3 * i + 2]);
      sum = sum + litestl::math::transformPoint(b.world, p);
      n++;
    }
  }
  if (n == 0) {
    return false;
  }
  out = sum * (1.0 / n);
  return true;
}

void ElementsDelta::undo(Context &ctx)
{
  for (int i = int(blocks_.size()) - 1; i >= 0; i--) {
    Status s = write(ctx, blocks_[i], blocks_[i].before);
    if (!s.ok()) {
      ctx.runtime().setStickyError(s); /* the history drops the step */
    }
  }
}

Status ElementsDelta::redo(Context &ctx)
{
  for (Block &b : blocks_) {
    CDCC_TRY(write(ctx, b, b.after));
  }
  return Status();
}

uint64_t ElementsDelta::memSize() const
{
  uint64_t n = sizeof(*this);
  for (const Block &b : blocks_) {
    n += sizeof(Block) + uint64_t(b.written.size()) * 52; /* handle and six doubles */
  }
  return n;
}

lbt::Struct<ElementsDelta> *ElementsDelta::defineBindings()
{
  static lbt::Struct<ElementsDelta> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ElementsDelta>("coredcc::ElementsDelta", sizeof(ElementsDelta));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    BIND_STRUCT_METHOD(st, changed, MARGS());
    BIND_STRUCT_METHOD(st, moved, MARGS());
    BIND_STRUCT_METHOD(st, mirrored, MARGS());
  }
  return st;
}

/* ---- registration ---- */

namespace {

CommandDef elementsDef(const char *name, const char *uiName)
{
  CommandDef def(name);
  const double z[] = {0, 0, 1};
  def.uiName(uiName)
      .input(PropertyDef::enumProp("kind")
                 .item("TRANSLATE", int64_t(DeltaKind::Translate))
                 .item("ROTATE", int64_t(DeltaKind::Rotate))
                 .item("SCALE", int64_t(DeltaKind::Scale)))
      .input(PropertyDef::quatProp("basis").defaultValue(PropValue::ofQuat(0, 0, 0, 1)))
      .input(PropertyDef::vecProp("pivot", 3)
                 .subtype(PropSubtype::Distance)
                 .unit(UnitFamily::Length))
      .input(PropertyDef::vecProp("vector", 3))
      .input(PropertyDef::vecProp("axis", 3).defaultValue(
          PropValue::ofComponents(PropValue::Type::Vec3, z)))
      .input(PropertyDef::floatProp("angle")
                 .subtype(PropSubtype::Angle)
                 .unit(UnitFamily::Angle))
      .input(PropertyDef::enumProp("domain")
                 .item("POINT", int64_t(ElementDomain::Point))
                 .item("EDGE", int64_t(ElementDomain::Edge))
                 .item("FACE", int64_t(ElementDomain::Face)))
      .input(PropertyDef::boolProp("symmetry"))
      .input(detail::objectsInput())
      .output(PropertyDef::intProp("skipped"))
      .output(PropertyDef::intProp("points"));
  return def;
}

} // namespace

Status registerElementCommands(Runtime &rt)
{
  CDCC_TRY(detail::addCommandOnce<ElementsDelta>(
      rt, elementsDef(kElementsApplyDeltaCommand, "Transform Elements")));
  CommandDef drag = elementsDef(kElementsDragDeltaCommand, "Transform Elements (Drag)");
  drag.flags(CommandFlags::Interactive | CommandFlags::Private);
  return detail::addCommandOnce<ElementsDelta>(rt, std::move(drag));
}

} // namespace coredcc

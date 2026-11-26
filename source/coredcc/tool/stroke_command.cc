/* core.stroke.apply: a brush's stroke as one Interactive command that replays its samples
 * on redo. */
#include "coredcc/api/ref_text.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/object.h"
#include "coredcc/scene/pick_scene.h"
#include "coredcc/scene/scene.h"
#include "coredcc/tool/elements.h"
#include "coredcc/tool/stroke.h"

#include "litestl/binding/binding.h"
#include "litestl/math/transform.h"

#include <cmath>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

double3 normalized(const double3 &v)
{
  const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return len == 0.0 ? v : v * (1.0 / len);
}

/* m's linear part times v, or its transpose's. */
double3 linear(const mat4d &m, const double3 &v, bool transposed)
{
  const double *d = m;
  double3 out;
  for (int r = 0; r < 3; r++) {
    out[r] = 0.0;
    for (int c = 0; c < 3; c++) {
      out[r] += (transposed ? d[r * 4 + c] : d[c * 4 + r]) * v[c];
    }
  }
  return out;
}

double3 reflect(double3 v, uint32_t axes)
{
  for (int k = 0; k < 3; k++) {
    if (axes & (1u << k)) {
      v[k] = -v[k];
    }
  }
  return v;
}

PointerSample sampleOf(const InputEvent &e)
{
  PointerSample s;
  s.x = e.x;
  s.y = e.y;
  s.pressure = e.pressure;
  s.tiltX = e.tiltX;
  s.tiltY = e.tiltY;
  s.twist = e.twist;
  s.time = e.time;
  return s;
}

} // namespace

Status StrokeCommand::canRun(Context &ctx) const
{
  CDCC_TRY(detail::sceneOf(ctx).status());
  return Status();
}

Status StrokeCommand::exec(Context &ctx)
{
  const detail::SceneOf s = *detail::sceneOf(ctx);
  Result<BlockHandle> object = detail::blockOf(ctx, "object");
  CDCC_TRY(object.status());
  if (!*object) {
    return Status::error(StatusCode::FailedPrecondition, "core.stroke.apply: no object");
  }
  Result<TransformState> st = computeState(*s.lib, *object, s.scene->time());
  CDCC_TRY(st.status());
  if (!litestl::math::tryInverse(st->world, inverse_)) {
    return Status::error(StatusCode::FailedPrecondition,
                         "core.stroke.apply: the object's matrix can't be inverted");
  }
  const PropertyBag &in = *ctx.inputs();
  StrokeSettings settings;
  settings.space = StrokeSpace(in.get("space")->asEnum());
  settings.method = StrokeMethod(in.get("method")->asEnum());
  settings.spacing = in.get("spacing")->asFloat();
  settings.radius = in.get("radius")->asFloat();
  settings.radiusIsWorld = in.get("radiusIsWorld")->asBool();
  CDCC_TRY(sampler_.configure(settings));

  target_.block = *object;
  target_.world = st->world;
  const PropValue pointer = detail::pointerTo(*s.lib, *object);
  target_.object =
      litestl::util::string(formatRef(ctx.runtime(), *pointer.tryPointer()).c_str());
  target_.mirrorAxes = 0;
  if (in.get("symmetry")->asBool()) {
    /* The data block's Symmetry, else the object's. */
    BlockHandle from = *object;
    if (const Transformable t = s.lib->query<Transformable>(*object)) {
      ObjectInfo info;
      ChannelBlock channels;
      t.read(info, channels);
      const BlockHandle data = s.lib->find(info.data);
      if (data && s.lib->query<Symmetry>(data)) {
        from = data;
      }
    }
    if (const Symmetry sym = s.lib->query<Symmetry>(from)) {
      target_.mirrorAxes = sym.axes() & 7u;
      CDCC_TRY(ctx.runtime().takeStickyError());
    }
  }
  const PropValue *brush = in.get("brush");
  Result<Owned<StrokeBrush>> made = ctx.runtime().ext<StrokeBrushRegistry>().make(
      ctx.runtime(), brush ? std::string_view(brush->asString()) : std::string_view());
  CDCC_TRY(made.status());
  brush_ = std::move(made).value();
  applied_.clear();
  CDCC_TRY(brush_->begin(ctx, target_));
  return ctx.runtime().takeStickyError();
}

Status StrokeCommand::update(Context &ctx, const ViewMath &view, const InputEvent &e)
{
  if (!view.valid()) {
    return Status::error(StatusCode::InvalidArgument,
                         "core.stroke.apply: the view isn't valid");
  }
  view_ = view;
  sampler_.setView(view);
  for (int32_t i = 0; i < e.sampleCount(); i++) {
    sampler_.push(e.sampleAt(i));
  }
  sampler_.push(sampleOf(e));
  return applyAll(ctx);
}

Status StrokeCommand::finish(Context &ctx, const ViewMath &view)
{
  if (view.valid()) {
    view_ = view;
    sampler_.setView(view);
  }
  sampler_.end();
  return applyAll(ctx);
}

Status StrokeCommand::applyAll(Context &ctx)
{
  const BlockHandle object = target_.block;
  const ViewMath &view = view_;
  /* The ray hits the target only, through pickScene (so: the last evaluateScene). */
  sampler_.setHitTest([&ctx, &view, object](double x, double y, double3 &p, double3 &n) {
    Result<detail::SceneOf> s = detail::sceneOf(ctx);
    if (!s.ok()) {
      return false;
    }
    PickOptions options;
    options.maxHits = 1;
    PickFilter filter;
    filter.accept = [object](BlockHandle h) { return h == object; };
    Vector<PickHit> hits;
    if (!pickScene(ctx, s->handle, view, x, y, options, filter, hits).ok() ||
        hits.size() == 0 || !hits[0].covers())
    {
      return false;
    }
    p = hits[0].point;
    const double3 &hn = hits[0].normal;
    const bool none = hn[0] == 0.0 && hn[1] == 0.0 && hn[2] == 0.0;
    n = none ? view.rayDirection(x, y) * -1.0 : hn; /* a bounds hit: face the eye */
    return true;
  });
  const int32_t n = sampler_.poll();
  sampler_.setHitTest(StrokeHitTest());
  for (int32_t i = 0; i < n; i++) {
    CDCC_TRY(applyOne(ctx, sampler_.sample(i)));
  }
  return Status();
}

Status StrokeCommand::applyOne(Context &ctx, const StrokeSample &s)
{
  const uint32_t axes = target_.mirrorAxes;
  for (uint32_t m = 0; m < 8; m++) {
    if ((m & axes) != m) {
      continue;
    }
    StrokeSample c = s;
    if (m != 0) {
      /* Reflected in the object's space: points through W⁻¹, normals through Wᵀ. */
      const mat4d &w = target_.world;
      c.world = litestl::math::transformPoint(
          w, reflect(litestl::math::transformPoint(inverse_, s.world), m));
      c.normal = normalized(linear(inverse_, reflect(linear(w, s.normal, true), m), true));
      c.dragWorld = linear(w, reflect(linear(inverse_, s.dragWorld, false), m), false);
      c.mirror = m;
    }
    applied_.append(c);
    CDCC_TRY(brush_->apply(ctx, c));
    CDCC_TRY(ctx.runtime().takeStickyError());
  }
  return Status();
}

void StrokeCommand::undo(Context &ctx)
{
  if (brush_) {
    brush_->undo(ctx);
  }
}

Status StrokeCommand::redo(Context &ctx)
{
  if (!brush_) {
    return Status::error(StatusCode::FailedPrecondition, "core.stroke.apply: no brush");
  }
  CDCC_TRY(brush_->begin(ctx, target_));
  CDCC_TRY(ctx.runtime().takeStickyError());
  for (const StrokeSample &s : applied_) {
    CDCC_TRY(brush_->apply(ctx, s));
    CDCC_TRY(ctx.runtime().takeStickyError());
  }
  return Status();
}

uint64_t StrokeCommand::byteSize() const
{
  return sizeof(*this) + (brush_ ? brush_->byteSize() : 0) +
         uint64_t(applied_.size()) * sizeof(StrokeSample);
}

lbt::Struct<StrokeCommand> *StrokeCommand::defineBindings()
{
  static lbt::Struct<StrokeCommand> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<StrokeCommand>("coredcc::StrokeCommand", sizeof(StrokeCommand));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, byteSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    BIND_STRUCT_METHOD(st, sampleCount, MARGS());
    BIND_STRUCT_METHOD(st, sampleAt, MARGS("i"));
  }
  return st;
}

/* ---- registration ---- */

namespace detail {

PropertyDef strokeSpaceProp()
{
  return PropertyDef::enumProp("space")
      .item("SCREEN", int64_t(StrokeSpace::Screen))
      .item("WORLD", int64_t(StrokeSpace::World));
}

PropertyDef strokeMethodProp()
{
  return PropertyDef::enumProp("method")
      .item("PATH", int64_t(StrokeMethod::Path))
      .item("ANCHORED", int64_t(StrokeMethod::Anchored))
      .item("DRAG_DOT", int64_t(StrokeMethod::DragDot));
}

} // namespace detail

Status registerStrokeCommand(Runtime &rt)
{
  const StrokeSettings d;
  CommandDef def(kApplyStrokeCommand);
  def.uiName("Stroke")
      .flags(CommandFlags::Interactive | CommandFlags::Private)
      .input(PropertyDef::stringProp("brush"))
      .input(PropertyDef::pointerProp("object", StructId()))
      .input(detail::strokeSpaceProp())
      .input(detail::strokeMethodProp())
      .input(PropertyDef::floatProp("spacing")
                 .defaultValue(PropValue::ofFloat(d.spacing))
                 .range(StrokeSettings::kMinSpacing, 100.0))
      .input(PropertyDef::floatProp("radius")
                 .defaultValue(PropValue::ofFloat(d.radius))
                 .range(StrokeSettings::kMinRadius, 1e9))
      .input(PropertyDef::boolProp("radiusIsWorld"))
      .input(PropertyDef::boolProp("symmetry"))
      .output(PropertyDef::intProp("samples"));
  return detail::addCommandOnce<StrokeCommand>(rt, std::move(def));
}

} // namespace coredcc

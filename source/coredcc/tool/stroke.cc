#include "coredcc/tool/stroke.h"

#include "coredcc/base/text.h"
#include "coredcc/tool/stroke_curve.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <algorithm>
#include <cmath>

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;
using stroke::Cubic;
using double2 = litestl::math::Vec<double, 2>;

namespace {

/** Centripetal: no cusps or loops on jittery, clustered input. */
constexpr double kAlpha = 0.5;

double lerpNum(double a, double b, double t)
{
  return a + (b - a) * t;
}

double3 normalized(const double3 &v)
{
  const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return len == 0.0 ? v : double3(v[0] / len, v[1] / len, v[2] / len);
}

} // namespace

/* ---- StrokeSampler ---- */

Status StrokeSampler::configure(const StrokeSettings &s)
{
  if (s.space != int32_t(StrokeSpace::Screen) && s.space != int32_t(StrokeSpace::World)) {
    return Status::error(StatusCode::InvalidArgument, "StrokeSettings: no such space");
  }
  if (s.method < int32_t(StrokeMethod::Path) || s.method > int32_t(StrokeMethod::DragDot)) {
    return Status::error(StatusCode::InvalidArgument, "StrokeSettings: no such method");
  }
  if (!(s.radius >= StrokeSettings::kMinRadius) || !std::isfinite(s.radius) ||
      !(s.spacing >= StrokeSettings::kMinSpacing) || !std::isfinite(s.spacing))
  {
    return Status::error(StatusCode::InvalidArgument,
                         "StrokeSettings: the radius must be finite and 1e-6 or more, and "
                         "the spacing finite and 0.01 or more");
  }
  settings_ = s;
  reset();
  return Status();
}

void StrokeSampler::push(const PointerSample &s)
{
  if (!ended_) {
    queue_.append(s);
  }
}

void StrokeSampler::end()
{
  ended_ = true;
}

void StrokeSampler::reset()
{
  queue_.clear();
  cps_.clear();
  out_.clear();
  carry_ = 0.0;
  distance_ = 0.0;
  dropped_ = 0;
  firstHit_ = false;
  lastHit_ = double3(0, 0, 0);
  hasPrev_ = false;
  rawEmitted_ = false;
  ended_ = false;
  done_ = false;
  hasAnchor_ = false;
}

int32_t StrokeSampler::poll()
{
  out_.clear();
  if (done_) {
    return 0;
  }
  for (const PointerSample &s : queue_) {
    ingest(s);
  }
  queue_.clear();
  if (ended_) {
    /* Path holds the last segment back; Anchored and DragDot gave one per sample. */
    const int n = int(cps_.size());
    if (settings_.method == int32_t(StrokeMethod::Path) && n >= 2) {
      emitSegment(n - 2, true);
    }
    done_ = true;
  }
  return int32_t(out_.size());
}

void StrokeSampler::ingest(const PointerSample &in)
{
  const Ray ray = view_.rayFromPixel(in.x, in.y);
  const double3 viewvec = ray.dir;
  const StrokeMethod method = StrokeMethod(settings_.method);
  ControlPoint cp;
  cp.screen = double2(in.x, in.y);
  cp.viewvec = viewvec;
  cp.in = in;
  if (method == StrokeMethod::Anchored && hasAnchor_) {
    /* Onto the plane through the anchor that faced the eye when it was made, not the live
     * (deforming) surface. */
    const double3 &n = anchor_.viewvec;
    const double denom = viewvec[0] * n[0] + viewvec[1] * n[1] + viewvec[2] * n[2];
    cp.world = anchor_.world;
    if (std::fabs(denom) > 1e-7) {
      const double3 d = anchor_.world - ray.origin;
      const double s = (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) / denom;
      cp.world = ray.origin + viewvec * s;
    }
    cp.normal = n;
  }
  else {
    double3 p, nrm;
    if (hit_ && hit_(in.x, in.y, p, nrm)) {
      cp.world = p;
      cp.normal = normalized(nrm);
      cp.hit = true;
      firstHit_ = true;
      lastHit_ = p;
    }
    else if (method == StrokeMethod::Anchored) {
      return; /* an anchored stroke can't begin off the object */
    }
    else if (firstHit_) {
      cp.world = synthesizeMiss(in.x, in.y);
      cp.normal = viewvec;
    }
    else if (settings_.space == int32_t(StrokeSpace::Screen)) {
      cp.world = ray.origin + viewvec;
      cp.normal = viewvec;
    }
    else {
      return; /* world spacing before any hit: no plane to put it on yet */
    }
  }

  if (method == StrokeMethod::Anchored) {
    if (!hasAnchor_) {
      anchor_ = cp;
      hasAnchor_ = true;
    }
    StrokeSample s = makeSample(
        anchor_.world, anchor_.screen[0], anchor_.screen[1], anchor_, anchor_, 0.0,
        anchor_.normal, anchor_.hit);
    s.time = in.time;
    s.dragX = cp.screen[0] - anchor_.screen[0];
    s.dragY = cp.screen[1] - anchor_.screen[1];
    s.dragWorld = cp.world - anchor_.world;
    emit(s);
    return;
  }
  if (method == StrokeMethod::DragDot) {
    emit(makeSample(cp.world, cp.screen[0], cp.screen[1], cp, cp, 0.0, cp.normal, cp.hit));
    return;
  }
  cps_.append(cp);
  if (!rawEmitted_) {
    /* The first point: one sample, not interpolated. */
    rawEmitted_ = true;
    emit(makeSample(cp.world, cp.screen[0], cp.screen[1], cp, cp, 0.0, cp.normal, cp.hit));
    return;
  }
  /* With a point to its right, the segment before it is fixed. */
  const int n = int(cps_.size());
  if (n >= 3) {
    emitSegment(n - 3, false);
  }
}

void StrokeSampler::emitSegment(int i, bool rightClamp)
{
  const ControlPoint &p1 = cps_[i];
  const ControlPoint &p2 = cps_[i + 1];
  const ControlPoint &p0 = i > 0 ? cps_[i - 1] : p1;
  const ControlPoint &p3 = rightClamp ? p2 : cps_[i + 2];
  const Cubic<2> screenB =
      stroke::catmullRomToBezier(p0.screen, p1.screen, p2.screen, p3.screen, kAlpha);
  const Cubic<3> worldB =
      stroke::catmullRomToBezier(p0.world, p1.world, p2.world, p3.world, kAlpha);
  const double3 middle = stroke::evalCubic(worldB, 0.5);

  double step;
  stroke::Walk walk;
  if (settings_.space == int32_t(StrokeSpace::World)) {
    const double r =
        settings_.radiusIsWorld ? settings_.radius : worldRadiusAt(middle, settings_.radius);
    /* At least half a device pixel at the segment's middle, as on the screen. */
    const double dpi = view_.view().dpiScale > 0.0 ? view_.view().dpiScale : 1.0;
    const double floor = 0.5 / dpi * view_.pixelSizeAt(middle);
    step = std::max(settings_.spacing * 2.0 * r, floor > 0.0 ? floor : 1e-5);
    walk = stroke::arcLengthWalk(worldB, step, carry_, 32, room());
  }
  else {
    /* A world radius in pixels first, or a sub-pixel spacing floods the stroke. */
    const double r = settings_.radiusIsWorld ? pixelRadiusAt(middle, settings_.radius)
                                             : settings_.radius;
    step = std::max(settings_.spacing * 2.0 * r * view_.view().dpiScale, kMinStepPixels);
    walk = stroke::arcLengthWalk(screenB, step, carry_, 32, room());
  }
  carry_ = walk.carry;
  dropped_ += walk.dropped;

  for (double t : walk.ts) {
    const double2 sp = stroke::evalCubic(screenB, t);
    StrokeSample s = makeSample(stroke::evalCubic(worldB, t),
                                sp[0],
                                sp[1],
                                p1,
                                p2,
                                t,
                                stroke::lerp(p1.normal, p2.normal, t),
                                t < 0.5 ? p1.hit : p2.hit);
    s.interpolated = true;
    distance_ += step;
    s.distance = distance_;
    s.step = step;
    if (hasPrev_) {
      s.angle = std::atan2(s.y - prev_.y, s.x - prev_.x);
    }
    emit(s);
  }
}

StrokeSample StrokeSampler::makeSample(const double3 &world,
                                       double x,
                                       double y,
                                       const ControlPoint &a,
                                       const ControlPoint &b,
                                       double t,
                                       const double3 &normal,
                                       bool hit) const
{
  StrokeSample s;
  s.x = x;
  s.y = y;
  s.world = world;
  s.normal = normalized(normal);
  s.hit = hit;
  s.pressure = lerpNum(a.in.pressure, b.in.pressure, t);
  s.tiltX = lerpNum(a.in.tiltX, b.in.tiltX, t);
  s.tiltY = lerpNum(a.in.tiltY, b.in.tiltY, t);
  s.twist = lerpNum(a.in.twist, b.in.twist, t);
  s.time = lerpNum(a.in.time, b.in.time, t);
  if (settings_.radiusIsWorld) {
    s.radiusWorld = settings_.radius;
    s.radiusPixels = pixelRadiusAt(world, settings_.radius);
  }
  else {
    s.radiusPixels = settings_.radius;
    s.radiusWorld = worldRadiusAt(world, settings_.radius);
  }
  s.distance = distance_;
  return s;
}

int32_t StrokeSampler::room() const
{
  return std::max(kMaxSamplesPerPoll - int32_t(out_.size()), 0);
}

void StrokeSampler::emit(StrokeSample s)
{
  if (int32_t(out_.size()) >= kMaxSamplesPerPoll) {
    dropped_++;
    return;
  }
  out_.append(s);
  prev_ = s;
  hasPrev_ = true;
}

double3 StrokeSampler::synthesizeMiss(double x, double y) const
{
  /* At the last hit's depth, under the cursor. */
  const ViewPoint p = view_.project(lastHit_);
  if (!p.behind) {
    Result<double3> w = view_.unproject(x, y, p.depth);
    if (w.ok()) {
      return *w;
    }
  }
  const Ray ray = view_.rayFromPixel(x, y);
  return ray.origin + ray.dir;
}

double StrokeSampler::worldRadiusAt(const double3 &p, double radiusPixels) const
{
  return radiusPixels * view_.pixelSizeAt(p);
}

double StrokeSampler::pixelRadiusAt(const double3 &p, double radiusWorld) const
{
  return radiusWorld / std::max(view_.pixelSizeAt(p), 1e-12);
}

/* ---- StrokeBrushRegistry ---- */

Status StrokeBrushRegistry::add(std::string_view name, StrokeBrushFactory factory)
{
  if (name.empty() || !factory) {
    return Status::error(StatusCode::InvalidArgument,
                         "a stroke brush needs a name and a factory");
  }
  if (has(name)) {
    return Status::error(
        StatusCode::AlreadyExists,
        (std::string("stroke brush '") + std::string(name) + "' exists").c_str());
  }
  names_.append(litestl::util::string(std::string(name).c_str()));
  factories_.append(std::move(factory));
  return Status();
}

bool StrokeBrushRegistry::has(std::string_view name) const
{
  for (const litestl::util::string &n : names_) {
    if (std::string_view(n.c_str()) == name) {
      return true;
    }
  }
  return false;
}

Result<Owned<StrokeBrush>> StrokeBrushRegistry::make(Runtime &rt, std::string_view name) const
{
  for (int i = 0; i < int(names_.size()); i++) {
    if (std::string_view(names_[i].c_str()) == name) {
      return factories_[i](rt);
    }
  }
  Status err = Status::error(
      StatusCode::NotFound,
      (std::string("no stroke brush '") + std::string(name) + "'").c_str());
  Suggester sg(name);
  for (const litestl::util::string &n : names_) {
    sg.consider(n.c_str());
  }
  sg.addTo(err);
  return err;
}

/* ---- bindings ---- */

double StrokeTarget::worldAt(int32_t i) const
{
  const double *d = world;
  return i >= 0 && i < 16 ? d[i] : 0.0;
}

lbt::Struct<StrokeSettings> *StrokeSettings::defineBindings()
{
  static lbt::Struct<StrokeSettings> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<StrokeSettings>("coredcc::StrokeSettings", sizeof(StrokeSettings));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, space);
    BIND_STRUCT_MEMBER(st, method);
    BIND_STRUCT_MEMBER(st, spacing);
    BIND_STRUCT_MEMBER(st, radius);
    BIND_STRUCT_MEMBER(st, radiusIsWorld);
  }
  return st;
}

lbt::Struct<StrokeSample> *StrokeSample::defineBindings()
{
  static lbt::Struct<StrokeSample> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<StrokeSample>("coredcc::StrokeSample", sizeof(StrokeSample));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, x);
    BIND_STRUCT_MEMBER(st, y);
    BIND_STRUCT_MEMBER(st, world);
    BIND_STRUCT_MEMBER(st, normal);
    BIND_STRUCT_MEMBER(st, hit);
    BIND_STRUCT_MEMBER(st, interpolated);
    BIND_STRUCT_MEMBER(st, pressure);
    BIND_STRUCT_MEMBER(st, tiltX);
    BIND_STRUCT_MEMBER(st, tiltY);
    BIND_STRUCT_MEMBER(st, twist);
    BIND_STRUCT_MEMBER(st, time);
    BIND_STRUCT_MEMBER(st, radiusPixels);
    BIND_STRUCT_MEMBER(st, radiusWorld);
    BIND_STRUCT_MEMBER(st, distance);
    BIND_STRUCT_MEMBER(st, step);
    BIND_STRUCT_MEMBER(st, angle);
    BIND_STRUCT_MEMBER(st, dragX);
    BIND_STRUCT_MEMBER(st, dragY);
    BIND_STRUCT_MEMBER(st, dragWorld);
    BIND_STRUCT_MEMBER(st, mirror);
  }
  return st;
}

lbt::Struct<StrokeTarget> *StrokeTarget::defineBindings()
{
  static lbt::Struct<StrokeTarget> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<StrokeTarget>("coredcc::StrokeTarget", sizeof(StrokeTarget));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, object);
    BIND_STRUCT_MEMBER(st, mirrorAxes);
    BIND_STRUCT_METHOD(st, worldAt, MARGS("i"));
  }
  return st;
}

} // namespace coredcc

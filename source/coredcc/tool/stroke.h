#pragma once

/** \file
 * Strokes (P4b): StrokeSampler turns pointer samples into evenly spaced samples along a
 * centripetal Catmull-Rom spline; a client's StrokeBrush applies them; the command
 * core.stroke.apply records every sample it applied, so that redo replays them; the tool
 * core.tool.stroke connects the three.
 *
 * StrokeSampler is a port of sculptcore's `brush/stroke_driver`. What differs is in
 * `docs/plans/2026-11-25-p4-tools.md` ("StrokeSampler and the stroke command" and the step 7
 * notes).
 */

#include "coredcc/api/context.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/draw/view.h"
#include "coredcc/lib/library.h"
#include "coredcc/tool/input.h"

#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <string_view>

namespace coredcc {

/** Whether spacing is measured on the screen or in the world. */
enum class StrokeSpace : int32_t { Screen = 0, World = 1 };

/**
 * - Path: samples along the spline, `spacing` apart.
 * - Anchored: every sample is at the first one (the anchor), with the drag from it to the
 *   cursor (StrokeSample::dragX, dragY and dragWorld). A stroke can't begin off the object.
 * - DragDot: one sample per pointer sample, where it is.
 */
enum class StrokeMethod : int32_t { Path = 0, Anchored = 1, DragDot = 2 };

struct StrokeSettings {
  StrokeSpace space = StrokeSpace::Screen;
  StrokeMethod method = StrokeMethod::Path;
  /** The distance between samples, as a fraction of the diameter (2 × radius). */
  double spacing = 0.1;
  /** Logical pixels; world units with `radiusIsWorld`. */
  double radius = 25.0;
  bool radiusIsWorld = false;

  /** The smallest that `configure` takes. However small, the samples are at least half
   * a device pixel apart (StrokeSampler::kMinStepPixels). */
  static constexpr double kMinSpacing = 0.01;
  static constexpr double kMinRadius = 1e-6;

  static litestl::binding::types::Struct<StrokeSettings> *defineBindings();
};

/** One sample of a stroke. */
struct StrokeSample {
  /** Device pixels, y down. */
  double x = 0.0, y = 0.0;
  /** In the world. Off the object: on the plane through the last hit, facing the eye, or
   * before any hit a point on the ray. */
  double3 world;
  /** The surface's unit normal; off the object, the ray's direction. */
  double3 normal;
  bool hit = false;
  /** On the spline between pointer samples (Path's samples but the first). */
  bool interpolated = false;
  double pressure = 0.5, tiltX = 0.0, tiltY = 0.0, twist = 0.0;
  /** PointerSample::time, interpolated. */
  double time = 0.0;
  /** The radius in logical pixels and in world units, at `world`. */
  double radiusPixels = 0.0, radiusWorld = 0.0;
  /** Along the stroke, in the units of the spacing (device pixels, or world units), and
   * the spacing that led here. */
  double distance = 0.0, step = 0.0;
  /** The direction of travel on the screen, radians, from atan2 of device pixels (y down).
   * 0 for the first sample. */
  double angle = 0.0;
  /** Anchored: from the anchor to the cursor, in device pixels and in the world. */
  double dragX = 0.0, dragY = 0.0;
  double3 dragWorld;
  /** The mirror axes this copy is reflected across (bits 1, 2, 4: the object's x, y and
   * z = 0 planes); 0 for the stroke itself. */
  uint32_t mirror = 0;

  static litestl::binding::types::Struct<StrokeSample> *defineBindings();
};

/** Where a stroke's ray hits the object at a device pixel: a world point and unit normal. */
using StrokeHitTest =
    litestl::util::function<bool(double x, double y, double3 &point, double3 &normal)>;

/**
 * Pointer samples in, stroke samples out. Push the samples of an event, then poll: it
 * returns how many samples are ready, and `sample(i)` reads them until the next poll. A
 * Path stroke holds one segment back, until the next pointer sample or `end` fixes its
 * tangent. Everything is in double.
 */
class StrokeSampler {
public:
  /** InvalidArgument for an unknown space or method, or a radius or spacing that is
   * below its minimum or isn't finite. Resets the stroke. */
  Status configure(const StrokeSettings &settings);
  const StrokeSettings &settings() const
  {
    return settings_;
  }
  /** The ray's target; without one, nothing is hit. */
  void setHitTest(StrokeHitTest hit)
  {
    hit_ = std::move(hit);
  }
  /** The view the next poll's pointer samples are in. */
  void setView(const ViewMath &view)
  {
    view_ = view;
  }
  void push(const PointerSample &s);
  /** The pointer went up: the next poll gives the last segment. */
  void end();
  void reset();
  /** At most kMaxSamplesPerPoll: the ones past it are dropped and counted (`dropped`). */
  int32_t poll();
  /** Samples are never nearer than this on the screen, nor than its size in the world at
   * the segment's middle in World space. */
  static constexpr double kMinStepPixels = 0.5;
  static constexpr int32_t kMaxSamplesPerPoll = 4096;
  /** Samples dropped since the stroke began, because a poll had too many. */
  int64_t dropped() const
  {
    return dropped_;
  }
  /** An empty sample out of range. */
  StrokeSample sample(int32_t i) const
  {
    return i >= 0 && i < int32_t(out_.size()) ? out_[i] : StrokeSample();
  }
  /** `end` was called and polled. */
  bool finished() const
  {
    return done_;
  }
  /** Anchored: the anchor, once there is one. */
  bool hasAnchor() const
  {
    return hasAnchor_;
  }

private:
  struct ControlPoint {
    litestl::math::Vec<double, 2> screen;
    double3 world, normal, viewvec;
    bool hit = false;
    PointerSample in;
  };

  void ingest(const PointerSample &s);
  void emitSegment(int i, bool rightClamp);
  StrokeSample makeSample(const double3 &world,
                          double x,
                          double y,
                          const ControlPoint &a,
                          const ControlPoint &b,
                          double t,
                          const double3 &normal,
                          bool hit) const;
  void emit(StrokeSample s);
  /* How many more this poll may give. */
  int32_t room() const;
  double3 synthesizeMiss(double x, double y) const;
  double worldRadiusAt(const double3 &p, double radiusPixels) const;
  double pixelRadiusAt(const double3 &p, double radiusWorld) const;

  StrokeSettings settings_;
  StrokeHitTest hit_;
  ViewMath view_;
  litestl::util::Vector<PointerSample> queue_;
  litestl::util::Vector<ControlPoint> cps_;
  litestl::util::Vector<StrokeSample> out_;
  double carry_ = 0.0;
  double distance_ = 0.0;
  int64_t dropped_ = 0;
  bool firstHit_ = false;
  double3 lastHit_;
  StrokeSample prev_;
  bool hasPrev_ = false;
  bool rawEmitted_ = false;
  bool ended_ = false;
  bool done_ = false;
  ControlPoint anchor_;
  bool hasAnchor_ = false;
};

/** What a brush is given when a stroke begins. */
struct StrokeTarget {
  /** The object, as a Pointer's text (formatRef: what scripts name blocks by), and its world
   * matrix (column-major) at the stroke's start. */
  litestl::util::string object;
  mat4d world;
  /** The mirror axes the stroke applies its samples across (Symmetry::axes). */
  uint32_t mirrorAxes = 0;
  /** For native brushes. */
  BlockHandle block;

  double worldAt(int32_t i) const;
  static litestl::binding::types::Struct<StrokeTarget> *defineBindings();
};

/**
 * What a client's brush implements. One instance per stroke; its record owns it, so undo and
 * redo work after the tool is gone.
 *
 * - `begin`: the stroke starts, and again before redo replays it. It may write values.
 * - `apply`: one sample, once for the stroke and once for each mirror (`mirror` set). It may
 *   only write values: it runs where commands can't be.
 * - `undo`: put back what begin and apply wrote, as a Command's undo.
 * - What they change must be notified, as any write: through values (setValue,
 * writeValue), `invalidateData` for geometry, or ChangeBus::notify (`notifyChanged` in
 * scripts). The scene's evaluation is incremental, so an unnotified block keeps its old
 * result.
 * - `byteSize`: its undo data, in bytes.
 */
struct StrokeBrush : IfaceRef {
  Status begin(Context &ctx, const StrokeTarget &target)
  {
    return CDCC_CALL(begin, ctx, target);
  }
  Status apply(Context &ctx, const StrokeSample &s)
  {
    return CDCC_CALL(apply, ctx, s);
  }
  void undo(Context &ctx)
  {
    CDCC_CALL(undo, ctx);
  }
  uint64_t byteSize() const
  {
    return CDCC_CALL(byteSize);
  }

  CDCC_INTERFACE(StrokeBrush, "coredcc::StrokeBrush")
  {
    BIND_STRUCT_METHOD(st, begin, MARGS("ctx", "target"));
    BIND_STRUCT_METHOD(st, apply, MARGS("ctx", "s"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, byteSize, MARGS());
  }
};

/** Makes a brush for one stroke. */
using StrokeBrushFactory = litestl::util::function<Result<Owned<StrokeBrush>>(Runtime &rt)>;

/** Brushes by name, for core.stroke.apply's `brush` input. A Runtime extension. */
class StrokeBrushRegistry {
public:
  static constexpr const char *kExtName = "coredcc.StrokeBrushRegistry";
  explicit StrokeBrushRegistry(Runtime *)
  {
  }
  /** AlreadyExists for a name that is taken; InvalidArgument for an empty one. */
  Status add(std::string_view name, StrokeBrushFactory factory);
  /** NotFound, with suggestions. */
  Result<Owned<StrokeBrush>> make(Runtime &rt, std::string_view name) const;
  bool has(std::string_view name) const;

private:
  litestl::util::Vector<litestl::util::string> names_;
  litestl::util::Vector<StrokeBrushFactory> factories_;
};

/** Registers a bound C++ class as a brush named `name`. */
template <class Impl> Status addNativeStrokeBrush(Runtime &rt, std::string_view name);

/**
 * The Interactive command `core.stroke.apply`. Inputs: `brush` (a StrokeBrushRegistry
 * name), `object` (the target; a Pointer), the StrokeSettings (`space`, `method`,
 * `spacing`, `radius`, `radiusIsWorld`) and `symmetry`. exec makes the brush and begins
 * it; a tool then feeds it with `update` and `finish` from updateInteractive. Output:
 * `samples`.
 */
inline constexpr const char *kApplyStrokeCommand = "core.stroke.apply";
inline constexpr const char *kStrokeTool = "core.tool.stroke";

class StrokeCommand {
public:
  /** Pushes the event's pointer samples (its coalesced ones, then its own) and applies what
   * the sampler gives. */
  Status update(Context &ctx, const ViewMath &view, const InputEvent &e);
  /** Ends the sampler and applies the rest. */
  Status finish(Context &ctx, const ViewMath &view);
  int32_t sampleCount() const
  {
    return int32_t(applied_.size());
  }
  StrokeSample sampleAt(int32_t i) const
  {
    return i >= 0 && i < sampleCount() ? applied_[i] : StrokeSample();
  }
  const StrokeSampler &sampler() const
  {
    return sampler_;
  }

  Status canRun(Context &ctx) const;
  Status exec(Context &ctx);
  void undo(Context &ctx);
  Status redo(Context &ctx);
  uint64_t byteSize() const;
  void discard()
  {
  }
  static litestl::binding::types::Struct<StrokeCommand> *defineBindings();

private:
  Status applyAll(Context &ctx);
  Status applyOne(Context &ctx, const StrokeSample &s);

  Owned<StrokeBrush> brush_;
  StrokeTarget target_;
  /** The last update's view, for the ray. */
  ViewMath view_;
  mat4d inverse_;
  StrokeSampler sampler_;
  litestl::util::Vector<StrokeSample> applied_;
};

/** Registers core.stroke.apply (idempotent). Needs the scene's commands. */
Status registerStrokeCommand(Runtime &rt);
/** core.stroke.apply and core.tool.stroke (idempotent); registerToolBuiltins calls it. */
Status registerStrokeBuiltins(Runtime &rt);

} // namespace coredcc

#include "coredcc/base/iface/registry.h"

namespace coredcc {

template <class Impl> Status addNativeStrokeBrush(Runtime &rt, std::string_view name)
{
  const std::string impl = std::string(name) + "#brush";
  CDCC_TRY((registerImpl<StrokeBrush, Impl>(rt, impl)));
  return rt.ext<StrokeBrushRegistry>().add(name, [impl](Runtime &r) {
    return own<StrokeBrush>(r, impl, litestl::alloc::New<Impl>("coredcc::StrokeBrush"));
  });
}

} // namespace coredcc

namespace litestl::binding {
/** StrokeSpace and StrokeMethod, bound as enums (tool/stroke.cc). */
template <> struct Binder<coredcc::StrokeSpace> {
  static const BindingBase *bind();
};
template <> struct Binder<coredcc::StrokeMethod> {
  static const BindingBase *bind();
};
} // namespace litestl::binding

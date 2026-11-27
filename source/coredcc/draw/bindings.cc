/* Binding descriptors for the view types. */
#include "coredcc/draw/drawlist.h"
#include "coredcc/draw/view.h"

#include "coredcc/base/detail/enum_binding.h"

#include "litestl/binding/binding.h"
#include "litestl/math/matrix_bindings.h"
#include "litestl/util/alloc.h"

using coredcc::detail::bindEnum;

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::DepthMode>::bind()
{
  using coredcc::DepthMode;
  static const BindingBase *e = bindEnum("coredcc::DepthMode",
                                         sizeof(DepthMode),
                                         {{"Test", int(DepthMode::Test)},
                                          {"Always", int(DepthMode::Always)},
                                          {"XRay", int(DepthMode::XRay)}});
  return e;
}

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::ShapeKind>::bind()
{
  using coredcc::ShapeKind;
  static const BindingBase *e = bindEnum("coredcc::ShapeKind",
                                         sizeof(ShapeKind),
                                         {{"Arrow", int(ShapeKind::Arrow)},
                                          {"Cone", int(ShapeKind::Cone)},
                                          {"Cube", int(ShapeKind::Cube)},
                                          {"Sphere", int(ShapeKind::Sphere)},
                                          {"Disc", int(ShapeKind::Disc)},
                                          {"Ring", int(ShapeKind::Ring)},
                                          {"Quad", int(ShapeKind::Quad)}});
  return e;
}

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::Handedness>::bind()
{
  using coredcc::Handedness;
  static const BindingBase *e =
      bindEnum("coredcc::Handedness",
               sizeof(Handedness),
               {{"Right", int(Handedness::Right)}, {"Left", int(Handedness::Left)}});
  return e;
}

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::UpAxis>::bind()
{
  using coredcc::UpAxis;
  static const BindingBase *e = bindEnum(
      "coredcc::UpAxis", sizeof(UpAxis), {{"Y", int(UpAxis::Y)}, {"Z", int(UpAxis::Z)}});
  return e;
}

namespace coredcc {

namespace lbt = litestl::binding::types;

lbt::Struct<View> *View::defineBindings()
{
  static lbt::Struct<View> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<View>("coredcc::View", sizeof(View));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, view);
    BIND_STRUCT_MEMBER(st, proj);
    BIND_STRUCT_MEMBER(st, reversedZ);
    BIND_STRUCT_MEMBER(st, infiniteFar);
    BIND_STRUCT_MEMBER(st, ortho);
    BIND_STRUCT_MEMBER(st, dpiScale);
    BIND_STRUCT_METHOD(st, viewportAt, MARGS("i"));
    BIND_STRUCT_METHOD(st, putViewport, MARGS("i", "value"));
    BIND_STRUCT_MEMBER(st, handedness);
    BIND_STRUCT_MEMBER(st, up);
    BIND_STRUCT_MEMBER(st, depthRange);
    BIND_STRUCT_METHOD(st, validate, MARGS());
  }
  return st;
}

lbt::Struct<ViewPoint> *ViewPoint::defineBindings()
{
  static lbt::Struct<ViewPoint> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ViewPoint>("coredcc::ViewPoint", sizeof(ViewPoint));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, x);
    BIND_STRUCT_MEMBER(st, y);
    BIND_STRUCT_MEMBER(st, depth);
    BIND_STRUCT_MEMBER(st, viewDistance);
    BIND_STRUCT_MEMBER(st, behind);
    BIND_STRUCT_MEMBER(st, visible);
  }
  return st;
}

lbt::Struct<ViewRegion> *ViewRegion::defineBindings()
{
  static lbt::Struct<ViewRegion> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ViewRegion>("coredcc::ViewRegion", sizeof(ViewRegion));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, planeCount, MARGS());
    BIND_STRUCT_METHOD(st, planeAt, MARGS("plane", "i"));
  }
  return st;
}

lbt::Struct<ViewMath> *ViewMath::defineBindings()
{
  static lbt::Struct<ViewMath> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ViewMath>("coredcc::ViewMath", sizeof(ViewMath));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, set, MARGS("view"));
    BIND_STRUCT_METHOD(st, valid, MARGS());
    BIND_STRUCT_METHOD(st, project, MARGS("p"));
    BIND_STRUCT_METHOD(st, unprojectTo, MARGS("x", "y", "depth", "out"));
    BIND_STRUCT_METHOD(st, rayOrigin, MARGS("x", "y"));
    BIND_STRUCT_METHOD(st, rayDirection, MARGS("x", "y"));
    BIND_STRUCT_METHOD(st, regionFromPixel, MARGS("x", "y", "radius"));
    BIND_STRUCT_METHOD(st, regionFromRect, MARGS("x0", "y0", "x1", "y1"));
    BIND_STRUCT_METHOD(st, pixelRowAt, MARGS("row", "i"));
    BIND_STRUCT_METHOD(st, nearPlaneAt, MARGS("i"));
    BIND_STRUCT_METHOD(st, farPlaneAt, MARGS("i"));
    BIND_STRUCT_METHOD(st, pixelSizeAt, MARGS("p"));
    BIND_STRUCT_METHOD(st, viewDistance, MARGS("p"));
  }
  return st;
}

lbt::Struct<DrawStyle> *DrawStyle::defineBindings()
{
  static lbt::Struct<DrawStyle> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<DrawStyle>("coredcc::DrawStyle", sizeof(DrawStyle));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, color);
    BIND_STRUCT_MEMBER(st, width);
    BIND_STRUCT_MEMBER(st, depthMode);
    BIND_STRUCT_MEMBER(st, id);
  }
  return st;
}

/* begin and end aren't bound: whoever hands a script the list owns its frame. */
lbt::Struct<DrawList> *DrawList::defineBindings()
{
  static lbt::Struct<DrawList> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<DrawList>("coredcc::DrawList", sizeof(DrawList));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, linesXyz, MARGS("style", "xyz"));
    BIND_STRUCT_METHOD(st, pointsXyz, MARGS("style", "xyz"));
    BIND_STRUCT_METHOD(st, trianglesXyz, MARGS("style", "xyz", "colors"));
    BIND_STRUCT_METHOD(st, shape, MARGS("style", "kind", "matrix"));
    BIND_STRUCT_METHOD(st, pushMatrix, MARGS("m"));
    BIND_STRUCT_METHOD(st, popMatrix, MARGS());
    BIND_STRUCT_METHOD(st, circle2d, MARGS("style", "x", "y", "radius", "filled"));
    BIND_STRUCT_METHOD(st, line2dXy, MARGS("style", "xy"));
    BIND_STRUCT_METHOD(st, recordCount, MARGS());
    BIND_STRUCT_METHOD(st, matrixDepth, MARGS());
    BIND_STRUCT_METHOD(st, frozen, MARGS());
    BIND_STRUCT_METHOD(st, address, MARGS());
    BIND_STRUCT_METHOD(st, byteSize, MARGS());
  }
  return st;
}

} // namespace coredcc

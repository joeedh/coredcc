/* Binding descriptors for the view types. */
#include "coredcc/draw/view.h"

#include "litestl/binding/binding.h"
#include "litestl/math/matrix_bindings.h"
#include "litestl/util/alloc.h"

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
    BIND_STRUCT_METHOD(st, setConventions, MARGS("depthRange", "hand", "upAxis"));
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
    BIND_STRUCT_MEMBER(st, distance);
    BIND_STRUCT_MEMBER(st, behind);
    BIND_STRUCT_MEMBER(st, visible);
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
    BIND_STRUCT_METHOD(st, pixelSizeAt, MARGS("p"));
    BIND_STRUCT_METHOD(st, viewDistance, MARGS("p"));
  }
  return st;
}

} // namespace coredcc

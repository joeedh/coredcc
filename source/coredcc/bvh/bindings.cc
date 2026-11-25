/* Binding descriptors for the BVH's value types. */
#include "coredcc/bvh/geometry_bvh.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;

template <> lbt::Struct<Box3d> *Box3d::defineBindings()
{
  static lbt::Struct<Box3d> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<Box3d>("coredcc::Box3d", sizeof(Box3d));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, empty, MARGS());
    BIND_STRUCT_METHOD(st, minAt, MARGS("axis"));
    BIND_STRUCT_METHOD(st, maxAt, MARGS("axis"));
    BIND_STRUCT_METHOD(st, put, MARGS("axis", "lo", "hi"));
  }
  return st;
}

lbt::Struct<RayHit> *RayHit::defineBindings()
{
  static lbt::Struct<RayHit> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<RayHit>("coredcc::RayHit", sizeof(RayHit));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, primitive);
    BIND_STRUCT_MEMBER(st, t);
    BIND_STRUCT_MEMBER(st, u);
    BIND_STRUCT_MEMBER(st, v);
    BIND_STRUCT_MEMBER(st, inside);
    BIND_STRUCT_MEMBER(st, backFacing);
  }
  return st;
}

lbt::Struct<NearestHit> *NearestHit::defineBindings()
{
  static lbt::Struct<NearestHit> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<NearestHit>("coredcc::NearestHit", sizeof(NearestHit));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, primitive);
    BIND_STRUCT_MEMBER(st, distance);
    BIND_STRUCT_MEMBER(st, point);
  }
  return st;
}

} // namespace coredcc

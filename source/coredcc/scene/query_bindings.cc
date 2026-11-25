/* Binding descriptors for what Spatials are asked. */
#include "coredcc/scene/nearest.h"
#include "coredcc/scene/pick.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;

lbt::Struct<RegionQuery> *RegionQuery::defineBindings()
{
  static lbt::Struct<RegionQuery> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<RegionQuery>("coredcc::RegionQuery", sizeof(RegionQuery));
    BIND_STRUCT_METHOD(st, planeCount, MARGS());
    BIND_STRUCT_METHOD(st, planeNormal, MARGS("i"));
    BIND_STRUCT_METHOD(st, planeOffset, MARGS("i"));
    BIND_STRUCT_METHOD(st, planeValues, MARGS());
  }
  return st;
}

lbt::Struct<PickOptions> *PickOptions::defineBindings()
{
  static lbt::Struct<PickOptions> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PickOptions>("coredcc::PickOptions", sizeof(PickOptions));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, radius);
    BIND_STRUCT_MEMBER(st, domains);
    BIND_STRUCT_MEMBER(st, shapeRadius);
    BIND_STRUCT_MEMBER(st, maxHits);
    BIND_STRUCT_MEMBER(st, everyDomain);
    BIND_STRUCT_MEMBER(st, occlusion);
    BIND_STRUCT_MEMBER(st, thinRadius);
    BIND_STRUCT_MEMBER(st, slope);
  }
  return st;
}

lbt::Struct<PickQuery> *PickQuery::defineBindings()
{
  static lbt::Struct<PickQuery> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PickQuery>("coredcc::PickQuery", sizeof(PickQuery));
    BIND_STRUCT_METHOD(st, planeCount, MARGS());
    BIND_STRUCT_METHOD(st, planeNormal, MARGS("i"));
    BIND_STRUCT_METHOD(st, planeOffset, MARGS("i"));
    BIND_STRUCT_METHOD(st, planeValues, MARGS());
    BIND_STRUCT_METHOD(st, hasRay, MARGS());
    BIND_STRUCT_METHOD(st, rayOrigin, MARGS());
    BIND_STRUCT_METHOD(st, rayDirection, MARGS());
    BIND_STRUCT_METHOD(st, rayLimit, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, wants, MARGS("domain"));
    BIND_STRUCT_METHOD(st, occlusion, MARGS());
    BIND_STRUCT_METHOD(st, radiusDevicePixels, MARGS());
    BIND_STRUCT_METHOD(st, surfaceRadiusDevicePixels, MARGS());
    BIND_STRUCT_METHOD(st, limitDepth, MARGS());
    BIND_STRUCT_METHOD(st, pixelSizeAt, MARGS("p"));
  }
  return st;
}

lbt::Struct<PickSink> *PickSink::defineBindings()
{
  static lbt::Struct<PickSink> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PickSink>("coredcc::PickSink", sizeof(PickSink));
    BIND_STRUCT_METHOD(st, element, MARGS("domain", "element"));
    BIND_STRUCT_METHOD(st, instance, MARGS("instance"));
    BIND_STRUCT_METHOD(st, point, MARGS("p", "pixels"));
    BIND_STRUCT_METHOD(st, segment, MARGS("a", "b", "pixels"));
    BIND_STRUCT_METHOD(st, triangle, MARGS("a", "b", "c"));
    BIND_STRUCT_METHOD(st, sphere, MARGS("center", "radius"));
    BIND_STRUCT_METHOD(st, capsule, MARGS("a", "b", "radius"));
    BIND_STRUCT_METHOD(st, box, MARGS("box"));
    BIND_STRUCT_METHOD(st, points, MARGS("domain", "elements", "values", "pixels"));
    BIND_STRUCT_METHOD(st, segments, MARGS("domain", "elements", "values", "pixels"));
    BIND_STRUCT_METHOD(st, triangles, MARGS("domain", "elements", "values"));
    BIND_STRUCT_METHOD(st, capsules, MARGS("domain", "elements", "values"));
    BIND_STRUCT_METHOD(st, any, MARGS());
  }
  return st;
}

lbt::Struct<NearQuery> *NearQuery::defineBindings()
{
  static lbt::Struct<NearQuery> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<NearQuery>("coredcc::NearQuery", sizeof(NearQuery));
    BIND_STRUCT_METHOD(st, point, MARGS());
    BIND_STRUCT_METHOD(st, radius, MARGS());
    BIND_STRUCT_METHOD(st, domain, MARGS());
    BIND_STRUCT_METHOD(st, maxCount, MARGS());
  }
  return st;
}

lbt::Struct<NearSink> *NearSink::defineBindings()
{
  static lbt::Struct<NearSink> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<NearSink>("coredcc::NearSink", sizeof(NearSink));
    BIND_STRUCT_METHOD(st, add, MARGS("element", "position"));
    BIND_STRUCT_METHOD(st, addAll, MARGS("elements", "positions"));
    BIND_STRUCT_METHOD(st, radius, MARGS());
  }
  return st;
}

} // namespace coredcc

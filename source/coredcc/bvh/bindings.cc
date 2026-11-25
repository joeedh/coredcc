/* Binding descriptors for the BVH's value types. */
#include "coredcc/bvh/box.h"

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

} // namespace coredcc

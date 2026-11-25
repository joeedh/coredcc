/* Binding descriptors for props' value types. */
#include "coredcc/props/bag.h"
#include "coredcc/props/value.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

const litestl::binding::BindingBase *
litestl::binding::Binder<coredcc::PropValue::Type>::bind()
{
  using T = coredcc::PropValue::Type;
  static types::Enum *e = nullptr;
  if (!e) {
    alloc::PermanentGuard guard;
    e = new types::Enum("coredcc::PropValueType", sizeof(T));
    for (int i = 0; i <= int(T::Pointer); i++) {
      e->addItem(coredcc::propValueTypeName(T(i)), i);
    }
  }
  return e;
}

namespace coredcc {

namespace lbt = litestl::binding::types;

const lbt::Struct<PropValue> *PropValue::defineBindings()
{
  static lbt::Struct<PropValue> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PropValue>("coredcc::PropValue", sizeof(PropValue));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, type, MARGS());
    BIND_STRUCT_METHOD(st, componentCount, MARGS());
  }
  return st;
}

const lbt::Struct<PropertyBag> *PropertyBag::defineBindings()
{
  static lbt::Struct<PropertyBag> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PropertyBag>("coredcc::PropertyBag", sizeof(PropertyBag));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
  }
  return st;
}

} // namespace coredcc

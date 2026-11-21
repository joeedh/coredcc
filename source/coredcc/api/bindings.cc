/* Binding descriptors for api types. */
#include "coredcc/api/context.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;

const lbt::Struct<Context> *Context::defineBindings()
{
  static lbt::Struct<Context> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<Context>("coredcc::Context", sizeof(Context));
  }
  return st;
}

} // namespace coredcc

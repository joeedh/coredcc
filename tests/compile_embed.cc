/* Embedding check (Windows): coredcc's public headers, and the litestl headers they
 * include, must compile after <windows.h> without NOMINMAX and next to Unreal-style
 * macros, as they would inside an Unreal editor module. The templates are instantiated
 * too, so macro clashes inside them show up. */
#include <windows.h>
#define check(expr) ((void)(expr))
#define verify(expr) ((void)(expr))
#define ensure(expr) (!!(expr))
#undef TEXT
#define TEXT(x) L##x

#include "coredcc/base/assert.h"
#include "coredcc/base/block_id.h"
#include "coredcc/base/flags.h"
#include "coredcc/base/handle_pool.h"
#include "coredcc/base/id.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/signal.h"
#include "coredcc/base/status.h"
#include "coredcc/base/text.h"
#include "coredcc/lib/block.h"
#include "coredcc/lib/library.h"
#include "coredcc/lib/provider.h"
#include "coredcc/lib/registry.h"
#include "coredcc/parse/invocation.h"
#include "coredcc/parse/path.h"
#include "coredcc/parse/path_cache.h"
#include "coredcc/props/bag.h"
#include "coredcc/props/def.h"
#include "coredcc/props/kind.h"
#include "coredcc/props/ref.h"
#include "coredcc/props/units.h"
#include "coredcc/props/value.h"

#include "test_util.h"

test_init;

namespace lbt = litestl::binding::types;

struct Sized : coredcc::IfaceRef {
  int size() const
  {
    return CDCC_CALL(size);
  }
  double weight() const
  {
    return CDCC_HAS(weight) ? CDCC_CALL(weight) : 1.0;
  }
  CDCC_INTERFACE(Sized, "embed.Sized")
  {
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_OPTIONAL_METHOD(st, weight, MARGS());
  }
};

struct Box {
  int n = 4;
  int size() const
  {
    return n;
  }
  static lbt::Struct<Box> *defineBindings()
  {
    auto *st = new lbt::Struct<Box>("embed::Box", sizeof(Box));
    BIND_STRUCT_MEMBER(st, n);
    BIND_STRUCT_METHOD(st, size, MARGS());
    return st;
  }
};

int main()
{
#if !defined(min) || !defined(max)
#error "windows.h min/max macros are not defined, so this test proves nothing"
#endif
  const wchar_t *unrealText = TEXT("text");
  test_assert(unrealText[0] == L't');
  {
    coredcc::Runtime rt;
    test_assert((coredcc::registerImpl<Sized, Box>(rt, "embed.Box").ok()));
    Box box;
    Sized s = coredcc::query<Sized>(rt, coredcc::TypeId::of("embed.Box"), &box);
    test_assert(s.size() == 4 && s.weight() == 1.0);
    coredcc::HandlePool<int> pool;
    coredcc::Signal<int> signal;
    coredcc::Library lib(&rt);
    test_assert(rt.ext<coredcc::PathCache>().get("a.b[\"c\"]").ok());
    test_assert(coredcc::coerce(coredcc::PropertyDef::floatProp("f").range(0, 1),
                                coredcc::PropValue::ofFloat(2))
                    .ok());
    test_assert(rt.validateAll().ok());
  }
  return test_end();
}

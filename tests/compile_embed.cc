/* Embedding check (Windows): coredcc's public headers, and the litestl headers they
 * include, must compile after <windows.h> without NOMINMAX and next to Unreal-style
 * check/verify/ensure macros, as they would inside an Unreal editor module. */
#include <windows.h>
#define check(expr) ((void)(expr))
#define verify(expr) ((void)(expr))
#define ensure(expr) (!!(expr))

#include "coredcc/base/assert.h"
#include "coredcc/base/handle_pool.h"
#include "coredcc/base/id.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/signal.h"
#include "coredcc/base/status.h"

#include "test_util.h"

test_init;

int main()
{
#if !defined(min) || !defined(max)
#error "windows.h min/max macros are not defined, so this test proves nothing"
#endif
  {
    coredcc::Runtime rt;
    test_assert(rt.validateAll().ok());
  }
  return test_end();
}

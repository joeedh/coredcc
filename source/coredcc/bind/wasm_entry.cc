/* The manager's export for a WASM module (Emscripten only): the TS runtime loads the
 * descriptors through it. */
#include "coredcc/bind/manager.h"

#include <emscripten/emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE litestl::binding::BindingManager *CDCC_GetManager()
{
  return coredcc::script::bindings();
}

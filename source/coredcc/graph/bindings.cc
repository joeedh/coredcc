/* Binding descriptors for graph types. */
#include "coredcc/graph/exec_context.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;

const lbt::Struct<NodeExecContext> *NodeExecContext::defineBindings()
{
  static lbt::Struct<NodeExecContext> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<NodeExecContext>("coredcc::NodeExecContext", sizeof(NodeExecContext));
  }
  return st;
}

} // namespace coredcc

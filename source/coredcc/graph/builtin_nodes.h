#pragma once

/** \file
 * Registering the built-in node types. Internal to graph/.
 */

#include "coredcc/base/assert.h"
#include "coredcc/graph/node_type.h"

#include <string>

namespace coredcc::detail {

template <Status (*Fn)(NodeExecContext &)>
void addBuiltin(Runtime &rt,
                NodeTypeRegistry &reg,
                NodeTypeDef &def,
                const std::string &name)
{
  Result<Owned<GraphKernel>> k = makeKernel<FnKernel<Fn>>(rt, name + "#kernel");
  CDCC_CHECK(k.ok(), "making a built-in kernel");
  def.kernel(std::move(k).value());
  const bool ok = reg.add(std::move(def)).ok();
  CDCC_CHECK(ok, "registering a built-in node type");
}

/** Constants and math. */
void addValueNodes(Runtime &rt, NodeTypeRegistry &reg);
/** Tables (graph/foreach.h). */
void addElementNodes(Runtime &rt, NodeTypeRegistry &reg);

} // namespace coredcc::detail

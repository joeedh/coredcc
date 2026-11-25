#pragma once

/** ile
 * Internal to the graph facades.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/graph/socket_type.h"
#include "coredcc/graph/value.h"

namespace coredcc::script::detail {

/** A socket's value for scripts. Unsupported for a payload; `what` and `name` are of the
 * socket. */
ValueResult toScript(const char *what, const string &name, const GraphValue &v);
/** A script's value for a socket. */
Result<GraphValue> fromScript(const SocketType &type, const string &name, const PropValue &value);
/** The graph block `path` names: itself, or through a pointer member. */
Result<BlockId> graphAt(Context &ctx, const string &path);

} // namespace coredcc::script::detail

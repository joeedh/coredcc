#pragma once

/** \file
 * Layouts for scripts. The rules of facades.h hold here too.
 */

#include "coredcc/bind/facades.h"

namespace coredcc::script {

/** The layout in `xml` as JSON (layoutToJson). An error has the position: "layout 3:14: ...". */
StringResult layoutJson(const string &xml);

/**
 * validateLayout's issues about the layout in `xml`, for `ctx`'s root, as a JSON array of
 * objects: `severity` ("Error" or "Warning"), `line` and `column` (from 1), `node` (its index in
 * preorder, the root's being 0), `code` (a StatusCode's name), `message`, and `suggestions` if
 * any. Text that isn't a layout is an error result, as layoutJson's.
 */
StringResult validateLayoutXml(Context &ctx, const string &xml);

/**
 * As `invoke`, with `prefix` as the path prefix ("" for none): the arguments that are paths
 * start from it, and so do the command's own, also in its undo and redo. Fails if `prefix`
 * doesn't name a struct now.
 */
BagResult invokeAt(Context &ctx, const string &prefix, const string &invocation);

} // namespace coredcc::script

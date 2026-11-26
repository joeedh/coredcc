#pragma once

/* The test app's box data type, `testapp.Box` (TA3): a cube of a half-size, with bounds
 * (Spatial, so it can be picked) and an overlay (OverlayDrawable, so it can be seen). */
#include "coredcc/api/context.h"
#include "coredcc/lib/library.h"

#include <string>

namespace testapp {

struct Box {
  double half = 0.5;
};

/** Registers the block type, its capabilities and its struct (`size`). */
void registerBoxes(coredcc::Runtime &rt);
/** A new box block (a GC root until an object uses it), as pointer text for an
 * invocation's `data` input; empty on failure. */
std::string createBox(coredcc::Context &ctx, double half);

} // namespace testapp

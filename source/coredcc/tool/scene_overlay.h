#pragma once

/** \file
 * Drawing the overlays of a scene's objects (OverlayDrawable).
 */

#include "coredcc/api/context.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/draw/view.h"

namespace coredcc {

/**
 * Calls `drawOverlay` of each object of the last evaluation of `scene` that is a member,
 * evaluated without an error and not hidden, and has an OverlayDrawable (its data
 * block's, else its own), with the object's `dataWorld` pushed. `state` has
 * OverlayState::Selected and Active. A null scene draws nothing.
 *
 * A foreign slot's exception is the Runtime's sticky error: the first one is taken and
 * returned, after the others drew.
 */
Status
drawSceneOverlays(Context &ctx, BlockHandle scene, const ViewMath &view, DrawList &out);

} // namespace coredcc

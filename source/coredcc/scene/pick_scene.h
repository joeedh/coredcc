#pragma once

/** \file
 * Picking in a scene: what is at or near a pixel, and what is in a part of the image.
 */

#include "coredcc/scene/pick.h"
#include "coredcc/scene/pick_filter.h"

#include "litestl/util/vector.h"

#include <cstdint>

namespace coredcc {

/**
 * The objects within a radius of a device pixel: the best hit of each, in the order that
 * detail::rankPicks describes. Thin shapes (points, segments and solids that are narrow
 * on the screen) come first, unless they are behind a surface under the pixel; then what
 * covers the pixel, by depth; then the rest.
 *
 * - The answer is for the last evaluateScene, which is what was drawn: its matrices and
 * bounds, and no object that came later. FailedPrecondition before the first one.
 * - Nothing beyond the far plane is found. The viewport's edge cuts nothing off.
 * - Each candidate's Spatial is asked now (its data block's, or its own). An object that
 * isn't Live any more, or has none, is left out. Without a `pick` slot, its local bounds'
 * surface answers. An object with empty bounds isn't found.
 * - Objects that are only parents of members, or whose channels don't fit, can't be
 * picked.
 * - While it runs, setValue, commands and evaluateScene are Busy, and it is too.
 * - The first exception of a foreign capability is returned, with the hits of the other
 * objects. With another error, there are none.
 */
Status pickScene(Context &ctx,
                 BlockHandle scene,
                 const ViewMath &view,
                 double x,
                 double y,
                 const PickOptions &options,
                 const PickFilter &filter,
                 litestl::util::Vector<PickHit> &hits);

/** What pickRegion found of one object. */
struct RegionHit {
  BlockHandle object;
  /** Its elements, as the object names them, in no particular order. Empty if objects
   * were asked for. */
  litestl::util::Vector<int32_t> elements;
};

/**
 * What may be in a part of the image, for a box or lasso selection: `region` is the
 * view's (ViewMath::regionFromRect), or any other planes. In the scene's order. The rules
 * of pickScene hold here too.
 *
 * - With a negative `domain`: objects. Those whose Spatial has `elementsInRegion`, if it
 * finds an element of any of its domains; the others, if their bounds may be there.
 * - With a domain: elements, of the objects whose Spatial has `elementsInRegion` and that
 * domain.
 * - What is found isn't outside any single plane, but may be outside of the region all
 * the same, as the Spatial decides. Things that are hidden by others are found.
 */
Status pickRegion(Context &ctx,
                  BlockHandle scene,
                  const ViewRegion &region,
                  int32_t domain,
                  const PickFilter &filter,
                  litestl::util::Vector<RegionHit> &hits);

} // namespace coredcc

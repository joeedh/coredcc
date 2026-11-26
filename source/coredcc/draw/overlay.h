#pragma once

/** \file
 * OverlayDrawable: a block that draws something of its own over the viewport, such as
 * a light's outline or a curve's handles (architecture §8, "Capabilities"). Found as
 * the other capabilities are, on an object's data block first, then on the object.
 */

#include "coredcc/base/flags.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/draw/view.h"

#include <cstdint>

namespace coredcc {

/** What the object is in the viewport, for drawOverlay. */
enum class OverlayState : uint32_t {
  None = 0,
  Selected = 1 << 0,
  Active = 1 << 1,
};
CDCC_FLAG_OPERATORS(OverlayState)

/**
 * `drawOverlay` appends records to `out`, in the block's own space: the caller has
 * pushed the object's matrix (ObjectEval::dataWorld). `state` is OverlayState bits. It
 * may not change anything, and in scripts every facade that writes is Busy in it.
 */
struct OverlayDrawable : IfaceRef {
  void drawOverlay(const ViewMath &view, uint32_t state, DrawList &out) const
  {
    CDCC_CALL(drawOverlay, view, state, out);
  }

  CDCC_INTERFACE(OverlayDrawable, "coredcc::OverlayDrawable")
  {
    BIND_STRUCT_METHOD(st, drawOverlay, MARGS("view", "state", "out"));
  }
};

} // namespace coredcc

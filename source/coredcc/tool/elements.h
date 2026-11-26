#pragma once

/** \file
 * Transforming elements (points, or the points of edges and faces) of the selected
 * objects' data blocks: the capabilities a block implements, TransformableElements and
 * Symmetry, and the commands core.element.applyDelta and core.element.dragDelta.
 */

#include "coredcc/base/iface/iface.h"
#include "coredcc/command/command.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/transform.h"

#include "litestl/util/vector.h"

#include <cstdint>

namespace coredcc {

/** What TransformableElements::gather gives: points, in the block's own space, each with
 * a weight (1 moves it all the way). A sink, as PickSink is. */
class ElementSet {
public:
  void add(int32_t handle, double x, double y, double z, double weight);
  int32_t size() const
  {
    return int32_t(handles.size());
  }
  void clear();

  litestl::util::Vector<int32_t> handles;
  /** Three numbers per point. */
  litestl::util::Vector<double> positions;
  litestl::util::Vector<double> weights;

  static litestl::binding::types::Struct<ElementSet> *defineBindings();
};

/**
 * A data block whose elements can be moved. It also implements ElementAccess, whose
 * `topoStamp` guards the undo and whose `positions` (of the Point domain) give the
 * positions of the points that symmetry writes.
 *
 * - `gather`: the points that a transform of `domain`'s selection moves, with weights.
 * - `setPositions`: three numbers per handle, in the block's own space. It may write the
 *   block's properties; the commands call `invalidateData(Positions)` after it.
 */
struct TransformableElements : IfaceRef {
  Status gather(ElementDomain domain, ElementSet &out) const
  {
    return CDCC_CALL(gather, domain, out);
  }
  Status setPositions(const litestl::util::Vector<int32_t> &handles,
                      const litestl::util::Vector<double> &positions)
  {
    return CDCC_CALL(setPositions, handles, positions);
  }

  CDCC_INTERFACE(TransformableElements, "coredcc::TransformableElements")
  {
    BIND_STRUCT_METHOD(st, gather, MARGS("domain", "out"));
    BIND_STRUCT_METHOD(st, setPositions, MARGS("handles", "positions"));
  }
};

/**
 * Mirror symmetry of a block's points.
 *
 * - `axes`: bits 1, 2 and 4 for mirroring across the block's x, y and z = 0 planes.
 * - `partners`: the mirror of each point across one axis (0, 1 or 2): itself for a point
 *   on the plane, -1 for none.
 */
struct Symmetry : IfaceRef {
  uint32_t axes() const
  {
    return CDCC_CALL(axes);
  }
  litestl::util::Vector<int32_t> partners(const litestl::util::Vector<int32_t> &handles,
                                          int32_t axis) const
  {
    return CDCC_CALL(partners, handles, axis);
  }

  CDCC_INTERFACE(Symmetry, "coredcc::Symmetry")
  {
    BIND_STRUCT_METHOD(st, axes, MARGS());
    BIND_STRUCT_METHOD(st, partners, MARGS("handles", "axis"));
  }
};

/**
 * `kind`, `basis`, `pivot`, `vector`, `axis` and `angle` as core.object.applyDelta's,
 * `domain` (POINT, EDGE, FACE) and `symmetry`. Works on the selected objects whose data
 * block (or the object itself) has TransformableElements, ElementAccess and points in the
 * domain's selection; the others are skipped. A point moves to W⁻¹·D(w)·W·p, where W is
 * the object's world matrix at the start and D(w) the delta weighted by the point's
 * weight. With `symmetry`, the block's Symmetry mirrors the moves to the partners that
 * aren't moved themselves (see `docs/plans/2026-11-25-p4-tools.md` for conflicts), and
 * points on a mirror plane stay on it. Outputs `skipped` and `points`.
 */
inline constexpr const char *kApplyElementDeltaCommand = "core.element.applyDelta";
/** The same, Interactive: a tool updates it with ElementsDelta::update. */
inline constexpr const char *kDragElementDeltaCommand = "core.element.dragDelta";

/** The instance of both commands. */
class ElementsDelta {
public:
  /** Applies `delta` to the points as they were at the start. */
  Status update(Context &ctx, const TransformDelta &delta);
  /** Whether a point is somewhere else than at the start. */
  bool changed() const;
  /** Points moved, and the partners written for them. */
  int32_t moved() const;
  int32_t mirrored() const;
  /** The mean position, in the world, of the points it moves: the gizmo's pivot. */
  bool center(double3 &out) const;

  Status canRun(Context &ctx) const;
  Status exec(Context &ctx);
  void undo(Context &ctx);
  Status redo(Context &ctx);
  uint64_t byteSize() const;
  void discard()
  {
  }
  static litestl::binding::types::Struct<ElementsDelta> *defineBindings();

  struct Block {
    BlockHandle block;
    TransformableElements elements;
    ElementAccess access;
    double topoStamp = 0.0;
    mat4d world, inverse;
    /* The moved points: handles, start positions, weights, and the mirror planes each
     * lies on (bits). Then the partners: handle, which moved point, and across which
     * axes. */
    litestl::util::Vector<int32_t> handles;
    litestl::util::Vector<double> start;
    litestl::util::Vector<double> weights;
    litestl::util::Vector<uint8_t> onPlanes;
    litestl::util::Vector<int32_t> partners;
    litestl::util::Vector<int32_t> partnerSource;
    litestl::util::Vector<uint8_t> partnerAxes;
    /* All written handles (moved, then partners), and their positions before and now. */
    litestl::util::Vector<int32_t> written;
    litestl::util::Vector<double> before;
    litestl::util::Vector<double> after;
  };

private:
  Status write(Context &ctx, Block &b, const litestl::util::Vector<double> &positions);

  litestl::util::Vector<Block> blocks_;
  int32_t skipped_ = 0;
};

namespace detail {
/** The block an object's elements are in: its data block if that has
 * TransformableElements, else the object if it has. Null for none. */
BlockHandle elementsBlock(const Library &lib, BlockHandle object);
/** Which moved point a partner gets, by the order the plan gives: the smallest set of
 * axes, then the lower axis bits, then the lower handle. `axes` is the block's. */
Status findPartners(const Symmetry &sym,
                    uint32_t axes,
                    const litestl::util::Vector<int32_t> &moved,
                    litestl::util::Vector<uint8_t> &onPlanes,
                    litestl::util::Vector<int32_t> &partners,
                    litestl::util::Vector<int32_t> &source,
                    litestl::util::Vector<uint8_t> &across);
} // namespace detail

/** Registers both commands (idempotent). Needs the scene's commands. */
Status registerElementCommands(Runtime &rt);

} // namespace coredcc

#pragma once

/** \file
 * What blocks can do in a scene. Any block type may implement these, natively or in a script
 * (architecture, "Capabilities"); coredcc's own Object implements Transformable.
 */

#include "coredcc/base/flags.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/invalidate.h"
#include "coredcc/bvh/box.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/nearest.h"
#include "coredcc/scene/pick.h"
#include "coredcc/scene/transform.h"

#include "litestl/util/vector.h"

#include <cstdint>

namespace coredcc {

class Context;

enum class ObjectFlags : uint32_t {
  None = 0,
  Hidden = 1 << 0,
  Unselectable = 1 << 1,
};
CDCC_FLAG_OPERATORS(ObjectFlags)

/** What Transformable::read says besides the channels. */
struct ObjectInfo {
  /** The transform type (TransformRegistry). */
  TypeId transformType;
  /** The parent, or null. */
  BlockId parent;
  /** The data block, or null. */
  BlockId data;
  /** ObjectFlags. */
  uint32_t flags = 0;

  static litestl::binding::types::Struct<ObjectInfo> *defineBindings();
};

/**
 * A block that is placed in the world.
 *
 * - `read` gives everything at once; `info` and `channels` hold other objects' values before.
 * - `writeChannels` stores channels. Callers use writeTransform, which also tells watchers.
 * - `setParentId` (optional) and `setTransformTypeId` (optional) store an id; the caller does
 *   the rest (writeParent, and writeTransform with the type's channels).
 */
struct Transformable : IfaceRef {
  void read(ObjectInfo &info, ChannelBlock &channels) const
  {
    CDCC_CALL(read, info, channels);
  }
  Status writeChannels(const ChannelBlock &channels)
  {
    return CDCC_CALL(writeChannels, channels);
  }
  bool hasSetParentId() const
  {
    return CDCC_HAS(setParentId);
  }
  Status setParentId(const BlockId &parent)
  {
    return CDCC_CALL(setParentId, parent);
  }
  bool hasSetTransformTypeId() const
  {
    return CDCC_HAS(setTransformTypeId);
  }
  Status setTransformTypeId(const TypeId &type)
  {
    return CDCC_CALL(setTransformTypeId, type);
  }

  CDCC_INTERFACE(Transformable, "coredcc::Transformable")
  {
    BIND_STRUCT_METHOD(st, read, MARGS("info", "channels"));
    BIND_STRUCT_METHOD(st, writeChannels, MARGS("channels"));
    BIND_OPTIONAL_METHOD(st, setParentId, MARGS("parent"));
    BIND_OPTIONAL_METHOD(st, setTransformTypeId, MARGS("type"));
  }
};

/** Kinds of elements, as numbers in Spatial and Elements. */
enum class ElementDomain : int32_t { Point = 0, Edge = 1, Face = 2 };

/**
 * Geometry that can be found by position. All coordinates are the block's own.
 *
 * - `localBounds`: a box around the geometry; empty for none.
 * - `domains`: bit d is set for each ElementDomain d it has.
 * - `closestElements`: gives the sink the elements of `query.domain()` within `query.radius()` of
 *   `query.point()`, or at least those that are nearest in the world. Others may be given too.
 * - `pick` (optional): gives the sink the shapes that may be near a pixel. Without it, the bounds
 *   are what can be picked.
 * - `elementsInRegion` (optional): the elements of a domain that may be in a region. Without it, a
 *   region selects the object, by its bounds.
 *
 * None of them may change anything.
 */
struct Spatial : IfaceRef {
  Box3d localBounds() const
  {
    return CDCC_CALL(localBounds);
  }
  uint32_t domains() const
  {
    return CDCC_CALL(domains);
  }
  void closestElements(const NearQuery &query, NearSink &sink) const
  {
    CDCC_CALL(closestElements, query, sink);
  }
  bool hasPick() const
  {
    return CDCC_HAS(pick);
  }
  void pick(const PickQuery &query, PickSink &sink) const
  {
    CDCC_CALL(pick, query, sink);
  }
  bool hasElementsInRegion() const
  {
    return CDCC_HAS(elementsInRegion);
  }
  litestl::util::Vector<int32_t> elementsInRegion(const RegionQuery &query, int32_t domain) const
  {
    return CDCC_CALL(elementsInRegion, query, domain);
  }

  CDCC_INTERFACE(Spatial, "coredcc::Spatial")
  {
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    BIND_OPTIONAL_METHOD(st, pick, MARGS("query", "sink"));
    BIND_OPTIONAL_METHOD(st, elementsInRegion, MARGS("query", "domain"));
  }
};

/** A block that keeps something made from its data, and has to hear of changes made by others.
 * Called by invalidateData only. */
struct Invalidation : IfaceRef {
  /** `kinds` are InvalidateKinds. */
  void invalidate(uint32_t kinds)
  {
    CDCC_CALL(invalidate, kinds);
  }

  CDCC_INTERFACE(Invalidation, "coredcc::Invalidation")
  {
    BIND_STRUCT_METHOD(st, invalidate, MARGS("kinds"));
  }
};

/**
 * Elements by handle.
 *
 * - `topoStamp` changes whenever handles may mean other elements.
 * - `positions`: three numbers per handle, in the block's own space.
 */
struct Elements : IfaceRef {
  double topoStamp() const
  {
    return CDCC_CALL(topoStamp);
  }
  int32_t elementCount(int32_t domain) const
  {
    return CDCC_CALL(elementCount, domain);
  }
  litestl::util::Vector<double> positions(const litestl::util::Vector<int32_t> &handles,
                                          int32_t domain) const
  {
    return CDCC_CALL(positions, handles, domain);
  }

  CDCC_INTERFACE(Elements, "coredcc::Elements")
  {
    BIND_STRUCT_METHOD(st, topoStamp, MARGS());
    BIND_STRUCT_METHOD(st, elementCount, MARGS("domain"));
    BIND_STRUCT_METHOD(st, positions, MARGS("handles", "domain"));
  }
};

/** Interface I of an object's data block, else of the object itself, else null. */
template <class I> I objectQuery(const Library &lib, BlockHandle object)
{
  if (const Transformable t = lib.query<Transformable>(object)) {
    ObjectInfo info;
    ChannelBlock channels;
    t.read(info, channels);
    if (I found = lib.query<I>(lib.find(info.data))) {
      return found;
    }
  }
  return lib.query<I>(object);
}

/** Tells everyone that a block's own data changed, other than through the datapath: its
 * Invalidation, if it has one, then the ChangeBus. `kinds` are InvalidateKinds. */
Status invalidateData(Context &ctx, BlockHandle block, InvalidateKinds kinds);

} // namespace coredcc

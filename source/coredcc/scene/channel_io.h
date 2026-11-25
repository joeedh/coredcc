#pragma once

/** \file
 * The channels of a transform as payload entries (files).
 */

#include "coredcc/lib/block_io.h"
#include "coredcc/scene/transform_registry.h"

#include "litestl/util/vector.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coredcc {
namespace detail {

/* How a def's value is in its channels. */

/** The channels of a def, or 0 for a kind that has none. */
int32_t channelsOf(PropKind kind);
void encodeChannels(const PropValue &v, int32_t first, ChannelBlock &out);
PropValue decodeChannels(const PropertyDef &def, int32_t first, const ChannelBlock &in);
/** Whether a double holds it. */
bool exactInt(int64_t v);

/** One entry for each channel def, by its name. An Enum is its item's key, since the numbers may be
 * another build's. `channels` must be of the type's size. */
void appendChannels(const TransformType &type,
                    const ChannelBlock &channels,
                    litestl::util::Vector<BlockEntry> &out);

/** Sets the channel that the entry is of, if it is one's value exactly (numbers may have become of
 * another type). */
bool takeChannel(const TransformType &type, const BlockEntry &e, ChannelBlock &out);

/** The type's defaults, with the entries over them. Those that no def takes stay in `entries`. Of two
 * with one name, the later counts. */
ChannelBlock takeChannels(const TransformType &type, litestl::util::Vector<BlockEntry> &entries);

/** Locks as text: the names of the defs whose channels are locked, or of one channel ("location.0"),
 * with spaces between. */
std::string lockNames(const TransformType &type, uint64_t locks);
/** The other way. Names that no channel has, or that can't be locked, go to `unknown`, with spaces. */
uint64_t lockMask(const TransformType &type, std::string_view names, std::string *unknown);

} // namespace detail
} // namespace coredcc

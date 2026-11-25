#pragma once

/** \file
 * Pointer values as text, for invocations, scripts and the test app:
 *
 *     @block(coredcc, 0123456789abcdef0123456789abcdef).modifiers["Bevel"] : Modifier
 *     @owner(tool).brushes[#3]
 *
 * Grammar (spaces and tabs allowed between the tokens):
 *
 *     ref    := '@' owner key* [ ':' name ]        the name of the addressed struct
 *     owner  := 'block' '(' name ',' hex32 ')'     a client's name and a LocalId
 *             | 'owner' '(' name ')'               a runtime owner
 *     key    := '.' member | '[' quoted ']' | '[' '#' integer ']'
 *     name   := ident ( '.' ident )* | quoted | '#' hex16
 *     member := ident | quoted
 *
 * `#hex16` is the id itself, for one whose name isn't registered here. The empty text (or
 * only spaces) is the null pointer.
 */

#include "coredcc/base/runtime.h"
#include "coredcc/props/ref.h"

#include "litestl/util/vector.h"

#include <span>
#include <string>
#include <string_view>

namespace coredcc {

/** The text form; "" for null. parseRef gives back an equal ref, if the block's client is
 * under its current name. */
std::string formatRef(const Runtime &rt, const StableRef &ref);

/**
 * Parses a ref's text. A client's alias becomes its current name. What it names needn't
 * exist: resolve() says whether it does. On error the Status names the offset, and
 * `errorOffset` (if given) receives it.
 */
Result<StableRef> parseRef(const Runtime &rt, std::string_view text, size_t *errorOffset = nullptr);

/* Block ids alone, for keys and for lists of blocks as String inputs:
 *
 *     coredcc,0123456789abcdef0123456789abcdef
 *
 *     id  := name ',' hex32
 *     ids := [ id ( ';' id )* [ ';' ] ]
 */

/** The most bytes parseBlockIds takes: about 1.5 million ids. */
inline constexpr size_t kMaxBlockIdsLength = size_t(1) << 26;

/** "" for a null id. */
std::string formatBlockId(const Runtime &rt, const BlockId &id);
/** A null id for the empty text. A client's alias becomes its current name. Errors as parseRef's. */
Result<BlockId> parseBlockId(const Runtime &rt,
                             std::string_view text,
                             size_t *errorOffset = nullptr);
/** None of `ids` is null. */
std::string formatBlockIds(const Runtime &rt, std::span<const BlockId> ids);
Result<litestl::util::Vector<BlockId>> parseBlockIds(const Runtime &rt,
                                                     std::string_view text,
                                                     size_t *errorOffset = nullptr);

} // namespace coredcc

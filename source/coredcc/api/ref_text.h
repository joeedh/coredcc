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

} // namespace coredcc

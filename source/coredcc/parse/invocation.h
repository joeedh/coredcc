#pragma once

/** \file
 * Invocation strings: a command id with named arguments, as scripts, keymaps and the
 * console write them (architecture §6).
 *
 *     mesh.extrude(dist=1.0 mode='REGION')
 *     view.frame
 *     object.transform(offset=(0, 0, 1.5), target="objects[\"Cube\"]")
 *
 * Grammar (spaces and tabs allowed between tokens):
 *
 *     invocation := id [ '(' [ arg ( sep arg )* ] ')' ]
 *     id         := ident ( '.' ident )*
 *     sep        := whitespace, or one ',' with optional whitespace around it
 *     arg        := ident '=' value
 *     value      := scalar | '(' [ scalar ( sep scalar )* ] ')' | '[' ... ']'
 *     scalar     := number | quoted | ident
 *
 * Values stay raw: the command layer converts them against the input's PropertyDef
 * (a quoted path becomes a Pointer there: one from the Context's prefix, or with a
 * leading `/` from its root). Tuples hold scalars only. Argument names are unique.
 */

#include "coredcc/base/status.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <string>
#include <string_view>

namespace coredcc {

struct RawScalar {
  enum class Kind : uint8_t { Number, String, Identifier };

  Kind kind = Kind::Number;
  /** The number as written (without a leading '+'), the string's contents, or the
   * identifier. */
  litestl::util::string text;
  /** Number only: no fraction or exponent. Integers fit int64. */
  bool isInteger = false;

  bool operator==(const RawScalar &b) const;
};

struct RawValue {
  bool isTuple = false;
  /** One item unless isTuple. */
  litestl::util::Vector<RawScalar> items;

  bool operator==(const RawValue &b) const;
};

struct InvocationArg {
  litestl::util::string name;
  RawValue value;
  /** Where the argument's name starts in the input, for error messages. */
  uint32_t offset = 0;
};

struct Invocation {
  litestl::util::string command;
  litestl::util::Vector<InvocationArg> args;

  const InvocationArg *find(std::string_view name) const;
  /** Equal command and arguments, in order (offsets are ignored). */
  bool operator==(const Invocation &b) const;
  /** The canonical spelling: `id(a=1 b="x" c=(1, 2))`, or just `id` with no arguments.
   * parseInvocation(toString()) gives back an equal Invocation. */
  std::string toString() const;
};

/** Parses an invocation; errors report an offset as parsePath does. */
Result<Invocation> parseInvocation(std::string_view text, size_t *errorOffset = nullptr);

} // namespace coredcc

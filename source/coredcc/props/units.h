#pragma once

/** \file
 * Unit families for numeric properties: formatting a value in a display unit, and
 * parsing user text back. Values are always stored in the family's base unit (meters,
 * radians, seconds, kilograms, factor). No expressions: one number and an optional
 * unit. Frames are in props/unit_settings.h.
 */

#include "coredcc/base/status.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace coredcc {

enum class UnitFamily : uint8_t { None, Length, Angle, Time, Mass, Factor };

const char *unitFamilyName(UnitFamily family);
/** The family of that name, if there is one. */
std::optional<UnitFamily> unitFamilyOfName(std::string_view name);

namespace units {

struct Unit {
  /** e.g. "centimeter". */
  const char *name;
  /** What format() prints and parse() reads, e.g. "cm". */
  const char *symbol;
  /** Base units per one of this unit. */
  double scale;
  /** Whether format() puts a space before the symbol ("1 m", but "90°", "50%"). */
  bool spaced;
  /** Further spellings parse() accepts, e.g. "deg", "degrees". Null-terminated. */
  const char *const *aliases;
};

/** The units of a family, base unit first. Empty for None. */
std::span<const Unit> unitsOf(UnitFamily family);

/** A unit of `family` by symbol, name, plural name or alias, or null. */
const Unit *findUnit(UnitFamily family, std::string_view text);

/**
 * `baseValue` in `unit` (a symbol or name; the base unit if unknown), with at most
 * `precision` decimals and trailing zeros removed, e.g. "1.25 m". Family None prints the
 * bare number.
 */
std::string format(double baseValue, UnitFamily family, std::string_view unit, int precision);

/**
 * Parses "1.5", "1.5 cm" or "-2e3mm" into base units. A bare number is in
 * `displayUnit`. Errors: no number, an unknown unit (with suggestions), trailing text, a
 * non-finite result, or an unknown `displayUnit`.
 */
Result<double> parse(std::string_view text, UnitFamily family, std::string_view displayUnit);

} // namespace units
} // namespace coredcc

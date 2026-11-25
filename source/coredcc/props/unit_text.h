#pragma once

/** \file
 * What units.cc and unit_settings.cc share. Internal.
 */

#include "coredcc/props/units.h"

namespace coredcc {
class UnitSettings;
}

namespace coredcc::units::detail {

/** Whether `text` is the symbol, name, plural name or an alias. */
bool spelledAs(const Unit &u, std::string_view text);
/** `v`, which is in `u`, with its symbol. */
std::string formatNumber(double v, const Unit &u, int precision);

/** A number, and what follows it ("" for nothing), without the spaces around it. */
struct Typed {
  double number = 0.0;
  std::string_view unit;
};
Result<Typed> parseNumber(std::string_view text);
/** `v`, or OutOfRange unless it is finite. */
Result<double> finite(double v, std::string_view text);

/** InvalidArgument, with suggestions from the units of the settings. */
Status
unknownUnit(UnitFamily family, std::string_view unit, const UnitSettings &settings);
/** From those of the family, without the frame. */
Status unknownUnit(UnitFamily family, std::string_view unit);

} // namespace coredcc::units::detail

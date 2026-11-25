#pragma once

/** \file
 * What units a document shows its values in, and its frame rate. Values are stored in
 * base units whatever these say (props/units.h).
 */

#include "coredcc/props/units.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coredcc {

/**
 * Frames per second, as a fraction in lowest terms: the broadcast rates aren't decimals.
 *
 * - Both numbers are 1 to 1,000,000, and the rate is at least 1/1000.
 * - The rounded names of the broadcast rates mean them: 29.97 (or 2997/100) is
 * 30000/1001, and 23.976 and 23.98 are 24000/1001. This is for multiples of 24 and 30, to
 * 240.
 */
class FrameRate {
public:
  static constexpr int32_t kMax = 1000000;

  FrameRate() = default;
  /** InvalidArgument for numbers below 1, OutOfRange for a rate that can't be held. */
  static Result<FrameRate> of(int64_t num, int64_t den = 1);
  /**
   * The fraction that `fps` is the double of, or else one that is nearly that: within
   * 2e-6 of it, relatively. `nearest(r.fps())` is `r`. Clamped to the range; 24 for NaN.
   */
  static FrameRate nearest(double fps);

  int32_t num() const
  {
    return num_;
  }
  int32_t den() const
  {
    return den_;
  }
  double fps() const
  {
    return double(num_) / double(den_);
  }
  bool operator==(const FrameRate &) const = default;

private:
  int32_t num_ = 24;
  int32_t den_ = 1;
};

/**
 * A display unit for each family, the frame rate, and the frame at time 0.
 *
 * - Time has one more unit here than in units::unitsOf: the frame ("f"), one over the
 * frame rate. As a unit it is a length of time, without `startFrame`.
 * - The defaults are meters, degrees, seconds, kilograms, factors, and 24 frames per
 * second.
 */
class UnitSettings {
public:
  FrameRate rate;
  double startFrame = 0.0;

  /** The seconds at a frame. */
  double timeOf(double frame) const
  {
    return (frame - startFrame) * rate.den() / rate.num();
  }
  /** The frame at a time in seconds. */
  double frameAt(double seconds) const
  {
    return seconds * rate.num() / rate.den() + startFrame;
  }

  /** How many units a family has, with the frame, which is the last of Time. */
  static int unitCount(UnitFamily family);
  /** `index` from 0 to unitCount; the base unit otherwise. For None, one without a
   * symbol. */
  units::Unit unitAt(UnitFamily family, int index) const;
  /** By symbol, name, plural name or alias; -1. */
  int findUnit(UnitFamily family, std::string_view text) const;
  /** Whether it is the frame. */
  static bool isFrame(UnitFamily family, int index);

  int displayIndex(UnitFamily family) const
  {
    return display_[int(family)];
  }
  units::Unit displayUnit(UnitFamily family) const
  {
    return unitAt(family, displayIndex(family));
  }
  /** OutOfRange for an index that isn't one. */
  Status setDisplayIndex(UnitFamily family, int index);
  /** As findUnit. InvalidArgument, with suggestions, for an unknown one. */
  Status setDisplayUnit(UnitFamily family, std::string_view text);

  bool operator==(const UnitSettings &) const = default;

private:
  static constexpr int kFamilies = 6;
  uint8_t display_[kFamilies] = {0, 0, 1, 0, 0, 0};
};

namespace units {

/** As the other `format`, in the settings' display unit. */
std::string
format(double baseValue, UnitFamily family, const UnitSettings &settings, int precision);
/** As the other `parse`: a bare number is in the settings' display unit, and `12f` is 12
 * frames. */
Result<double>
parse(std::string_view text, UnitFamily family, const UnitSettings &settings);

} // namespace units
} // namespace coredcc

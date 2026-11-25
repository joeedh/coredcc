#pragma once

#include "litestl/util/vector.h"

#include <cstdint>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

/** The channels of one transform: a flat array of doubles, in the order of its type's
 * channel defs. */
class ChannelBlock {
 public:
  ChannelBlock() = default;
  explicit ChannelBlock(int size)
  {
    resize(size);
  }

  int32_t size() const
  {
    return int32_t(values_.size());
  }
  /** 0 out of range. */
  double get(int32_t i) const
  {
    return i >= 0 && i < size() ? values_[i] : 0.0;
  }
  /** Nothing out of range. */
  void set(int32_t i, double v)
  {
    if (i >= 0 && i < size()) {
      values_[i] = v;
    }
  }
  /** New values are 0. */
  void resize(int32_t size)
  {
    const int32_t old = int32_t(values_.size());
    values_.resize(size_t(size > 0 ? size : 0));
    for (int32_t i = old; i < int32_t(values_.size()); i++) {
      values_[i] = 0.0;
    }
  }
  const double *data() const
  {
    return values_.data();
  }
  /** Bitwise, so NaN equals itself. */
  bool operator==(const ChannelBlock &b) const;

  static litestl::binding::types::Struct<ChannelBlock> *defineBindings();

 private:
  litestl::util::Vector<double, 16> values_;
};

/**
 * Everything a transform model may read besides its parent's state: channel values and the
 * time. It has no Library, Runtime or Context, so an evaluation is a pure function and the
 * graph can cache it.
 */
class EvalCtx {
 public:
  /** `channels` must outlive the EvalCtx. `time` is in seconds. */
  explicit EvalCtx(const ChannelBlock &channels, double time = 0.0)
      : channels_(&channels), time_(time)
  {
  }
  /** The same with overrides: channel i is `overrides[i]` where `overridden[i]` isn't 0. Both
   * have an entry for each channel and must outlive the EvalCtx. */
  EvalCtx(const ChannelBlock &channels,
          double time,
          const ChannelBlock &overrides,
          const litestl::util::Vector<uint8_t> &overridden)
      : channels_(&channels), overrides_(&overrides), overridden_(&overridden), time_(time)
  {
  }

  int32_t channelCount() const
  {
    return channels_->size();
  }
  /** The override if one is set, else the stored value. 0 out of range. */
  double channel(int32_t i) const
  {
    return isOverridden(i) ? overrides_->get(i) : channels_->get(i);
  }
  /** The stored value. */
  double stored(int32_t i) const
  {
    return channels_->get(i);
  }
  bool isOverridden(int32_t i) const
  {
    return overridden_ && i >= 0 && i < int32_t(overridden_->size()) && (*overridden_)[i] != 0;
  }
  bool isLocked(int32_t i) const
  {
    return i >= 0 && i < kMaxLocks && ((locks_ >> i) & 1) != 0;
  }
  /** Whether a solver has to leave it as it is: locked, or overridden. */
  bool isFixed(int32_t i) const
  {
    return isLocked(i) || isOverridden(i);
  }
  bool hasFixed() const
  {
    return locks_ != 0 || overridden_ != nullptr;
  }
  /** The same with locks: bit i is channel i's. Those after the first 64 can't be locked. */
  EvalCtx withLocks(uint64_t locks) const
  {
    EvalCtx c = *this;
    c.locks_ = locks;
    return c;
  }
  /** The same with other stored channels, which must outlive it. */
  EvalCtx withChannels(const ChannelBlock &channels) const
  {
    EvalCtx c = *this;
    c.channels_ = &channels;
    return c;
  }
  static constexpr int32_t kMaxLocks = 64;
  /** At least 0.5. NaN is false. */
  bool boolChannel(int32_t i) const
  {
    return channel(i) >= 0.5;
  }
  /** Rounded to nearest, halves away from zero, within +-2^53. NaN is 0. */
  int64_t intChannel(int32_t i) const;
  /** For an enum whose values are 0 to count - 1: any other value is 0, its first item. */
  int32_t enumChannel(int32_t i, int32_t count) const
  {
    const int64_t v = intChannel(i);
    return v >= 0 && v < count ? int32_t(v) : 0;
  }
  double time() const
  {
    return time_;
  }
  /** A copy of the channels, with the overrides applied. */
  ChannelBlock snapshot() const;

  static litestl::binding::types::Struct<EvalCtx> *defineBindings();

 private:
  const ChannelBlock *channels_;
  const ChannelBlock *overrides_ = nullptr;
  const litestl::util::Vector<uint8_t> *overridden_ = nullptr;
  uint64_t locks_ = 0;
  double time_ = 0.0;
};

namespace channels {
/** Channel values holding integers (Int, Enum, Bool) are exact up to this. */
constexpr double kMaxInt = 9007199254740992.0;
/** Rounded to nearest, clamped to +-kMaxInt. NaN is 0. */
int64_t toInt(double v);
} // namespace channels

} // namespace coredcc

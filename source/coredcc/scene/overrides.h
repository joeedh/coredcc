#pragma once

#include "coredcc/lib/library.h"
#include "coredcc/scene/eval_ctx.h"

#include <cstdint>

namespace coredcc {

class TransformType;

/** Values of some of an object's channels that come from elsewhere than the object. */
class ChannelOverrides {
 public:
  /** None, of `count` channels. */
  void reset(int32_t count)
  {
    if (any_ || values.size() != count) {
      values = ChannelBlock(count);
      mask.resize(size_t(count));
      for (uint8_t &m : mask) {
        m = 0;
      }
      any_ = false;
    }
  }
  /** Any value: also one that isn't finite. Nothing out of range. */
  void set(int32_t channel, double value)
  {
    if (channel >= 0 && channel < values.size()) {
      values.set(channel, value);
      mask[channel] = 1;
      any_ = true;
    }
  }
  bool any() const
  {
    return any_;
  }

  /* What EvalCtx takes. */
  ChannelBlock values;
  litestl::util::Vector<uint8_t> mask;

 private:
  bool any_ = false;
};

/**
 * Who drives channels of a Library's objects: animation curves, or a graph. The one way for that:
 * evaluateScene, computeState and the commands all ask it, and so see the same values.
 *
 * - `overridesOf` sets the channels of `object`, of the type `type`, that are driven at `time` (in
 *   seconds), and leaves the others. `out` comes without any.
 * - It answers for the document as it is now, and for the time it is asked for. If it can't (it only
 *   has another time's values, say), it fails, and doesn't leave the channels out.
 * - It may be asked about any Transformable, also one of no scene, and several times for the same one.
 *   Asking again gives the same until the document changes.
 * - It changes nothing in the document, and doesn't evaluate a scene. It is asked while evaluateScene
 *   evaluates, so commands and edits are Busy for it then.
 * - An error makes the object's status ModelFailed in evaluateScene, which returns the first one; a
 *   command fails with it, or leaves the object out if the code is FailedPrecondition.
 *
 * A driven channel's stored value stays what it is: the commands don't write it.
 */
class OverrideProvider {
 public:
  virtual ~OverrideProvider() = default;
  virtual Status overridesOf(const Library &lib,
                             BlockHandle object,
                             const TransformType &type,
                             double time,
                             ChannelOverrides &out) = 0;
};

/** Makes it the Library's provider until clearOverrideProvider, which has to come before its end.
 * AlreadyExists if the Library has one; Busy while a scene or a graph evaluates. Posts that anything may
 * have changed. */
Status setOverrideProvider(Library &lib, OverrideProvider &provider);
/** Nothing if it isn't the Library's provider. */
void clearOverrideProvider(Library &lib, OverrideProvider &provider);

namespace detail {

/** The Library's provider, or null. */
OverrideProvider *overrideProvider(const Library &lib);

/** What drives an object. The only caller of the provider: an exception that it left is the error. */
Status overridesOf(const Library &lib,
                   BlockHandle object,
                   const TransformType *type,
                   double time,
                   ChannelOverrides &out);
/** The same, with the provider looked up before. */
Status overridesOf(OverrideProvider *provider,
                   const Library &lib,
                   BlockHandle object,
                   const TransformType *type,
                   double time,
                   ChannelOverrides &out);

} // namespace detail
} // namespace coredcc

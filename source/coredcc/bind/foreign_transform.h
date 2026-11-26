#pragma once

/** \file
 * Transform types whose model is a script's.
 *
 * **Inside a model's slot** everything that takes a Context is Busy: the state of an
 * object depends on its channels, its parent's state and the time, and on nothing else.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/scene/transform_registry.h"

namespace coredcc::script {

/** A new transform type `id` (dotted) without channels. Owned by the caller, who disposes
 * of it: registration copies it. Names and channels are checked when it is registered. */
TransformTypeInfo *newTransformTypeDef(const string &id);
Status transformTypeDefUiName(TransformTypeInfo &type, const string &s);
/** What TransformTypeInfo::version says. A type that raises it has an `upgrade` slot. */
Status transformTypeDefVersion(TransformTypeInfo &type, int32_t version);
/** A channel def whose kind and default come from `defaultValue`: Bool, Int, Float,
 * Vec2-4 or Quat. A vector is as many channels as it has components. `distance` is for
 * lengths that move the object: a Float or a vector. */
Status transformTypeDefChannel(TransformTypeInfo &type,
                               const string &name,
                               const PropValue &defaultValue,
                               bool distance);
/** An Enum channel without items. */
Status transformEnumChannel(TransformTypeInfo &type, const string &name);
/** An item of one. `value` is a whole number within +-2^53. */
Status transformTypeDefChannelItem(TransformTypeInfo &type,
                                   const string &name,
                                   const string &key,
                                   double value,
                                   const string &uiName,
                                   const string &description);
/** `what`, as pathMeta names it: `uiName`, `description`, `icon`, `subtype`, `unit`,
 * `displayUnit` (after `unit`), or `default`, an Enum's key (after its items). */
Status transformTypeDefChannelText(TransformTypeInfo &type,
                                   const string &name,
                                   const string &what,
                                   const string &text);
/** `what`: `range`, `softRange` (a to b), `step` or `precision` (a). */
Status transformTypeDefChannelNumbers(
    TransformTypeInfo &type, const string &name, const string &what, double a, double b);

/**
 * Registers a copy of `type` with the script object `model`, which implements the
 * space-separated TransformModel slots in `slots`: `evaluate`, `setWorld` and optionally
 * `applyDelta`, `pivot`, `evaluateMany` and `upgrade`.
 *
 * - Takes one count of `model`, whatever the outcome, and releases it on failure, else
 * with the Runtime.
 * - The `core.` namespace is reserved.
 * - Busy while a scene or a graph evaluates.
 * - A refused registration leaves nothing behind.
 */
Status registerForeignTransformType(Context &ctx,
                                    const TransformTypeInfo &type,
                                    uint32_t model,
                                    const string &slots);

} // namespace coredcc::script

#pragma once

/** \file
 * Transform types whose model is a script's.
 *
 * **Inside a model's slot** everything that takes a Context is Busy: the state of an object
 * depends on its channels, its parent's state and the time, and on nothing else.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/scene/transform_registry.h"

namespace coredcc::script {

/** A new transform type `id` (dotted) without channels. Owned by the caller, who disposes of it:
 * registration copies it. Names and channels are checked when it is registered. */
TransformTypeInfo *newTransformType(const string &id);
Status transformTypeUiName(TransformTypeInfo &type, const string &s);
/** A channel def whose kind and default come from `defaultValue`: Bool, Int, Float or Vec2-4. A
 * vector is as many channels as it has components. `distance` is for lengths that move the object:
 * a Float or a vector. */
Status transformChannel(TransformTypeInfo &type,
                        const string &name,
                        const PropValue &defaultValue,
                        bool distance);

/**
 * Registers a copy of `type` with the script object `model`, which implements the space-separated
 * TransformModel slots in `slots`: `evaluate`, `setWorld` and optionally `applyDelta`.
 *
 * - Takes one count of `model`, whatever the outcome, and releases it on failure, else with the
 *   Runtime.
 * - The `core.` namespace is reserved.
 * - Busy while a scene or a graph evaluates.
 * - A refused registration leaves nothing behind.
 */
Status registerForeignTransformType(Context &ctx,
                                    const TransformTypeInfo &type,
                                    uint32_t model,
                                    const string &slots);

} // namespace coredcc::script

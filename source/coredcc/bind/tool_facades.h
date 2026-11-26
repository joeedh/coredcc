#pragma once

/** \file
 * Facades of tools for scripts: declaring and registering script tools, driving a
 * viewport's ToolManager (the Context's), modals and gizmos from a tool's slots, and the
 * script half of drags (`updateDrag`). See docs/plans/2026-11-25-p4-tools.md, "Bindings
 * and TS".
 *
 * Script modals and gizmos reach C++ as foreign handles, each carrying one count that the
 * facade takes, whatever the outcome; `slots` names the slots the object implements,
 * space-separated.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/draw/drawlist.h"
#include "coredcc/scene/transform.h"
#include "coredcc/tool/input.h"
#include "coredcc/tool/tool.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <span>

namespace litestl::binding {
struct BindingManager;
}

namespace coredcc::script {

using litestl::util::string;

/* ---- declaring tools ---- */

/** A new def for tool `id` (dotted). Owned by the caller; registration copies it. */
ToolDef *newToolDef(const string &id);
Status toolDefUiName(ToolDef &def, const string &s);
Status toolDefDescription(ToolDef &def, const string &s);
/** A setting whose kind and default come from `defaultValue`, as commandDefInput's. */
Status toolDefSetting(ToolDef &def, const string &name, const PropValue &defaultValue);
/** An enum setting of the space-separated `keys`, valued 0, 1, ...; the first is the
 * default. */
Status toolDefEnumSetting(ToolDef &def, const string &name, const string &keys);
Status toolDefLayout(ToolDef &def, const string &xml);
/**
 * Registers a copy of `def` as a script tool whose instances the factory `factory`
 * makes, implementing the Tool slots in `slots`. Takes one count of `factory` either
 * way. The `core.` namespace is reserved.
 */
Status registerForeignTool(Context &ctx,
                           const ToolDef &def,
                           uint32_t factory,
                           const string &slots);
/**
 * Registers a script StrokeBrush named `name` for core.stroke.apply's `brush` input:
 * `factory` makes one instance per stroke, implementing the StrokeBrush slots in `slots`.
 * Takes one count of `factory` either way. The `core.` namespace is reserved.
 */
Status registerForeignStrokeBrush(Context &ctx,
                                  const string &name,
                                  uint32_t factory,
                                  const string &slots);

/* ---- the Context's ToolManager ---- */

Status setActiveTool(Context &ctx, const string &id);
/** An EventResult, as a number. An error of a gizmo's update or hit test
 * (ToolManager::dispatch's `late`) is the status, with the result still the value. */
IntResult sendToolEvent(Context &ctx, const View &view, const InputEvent &e);
/** `out` has what was drawn even when this fails. */
Status drawTools(Context &ctx, const View &view, DrawList &out);
Status cancelTool(Context &ctx);
StringResult toolStatus(Context &ctx);
Status setToolStatus(Context &ctx, const string &text);
Status setToolOutcome(Context &ctx, const string &text);
/** The active tool's id, or "". */
StringResult activeTool(Context &ctx);

/* ---- from a tool's slots ---- */

/** Pushes the script modal `modal`. With `ownsInteractive`, the open interactive command
 * is its (ToolManager::pushModal). */
Status pushModal(Context &ctx, uint32_t modal, const string &slots, bool ownsInteractive);
/** Adds a built-in gizmo: "transform", which follows the settings of the tool
 * `settingsOf` (mode, pivot, orientation, target and so on, as core.tool.transform has
 * them; a missing one is its default), or core.tool.transform's for "". Its id. */
IntResult addGizmo(Context &ctx, const string &kind, const string &settingsOf);
/** Adds the script gizmo `gizmo`. Its id. */
IntResult addForeignGizmo(Context &ctx, uint32_t gizmo, const string &slots);
Status removeGizmo(Context &ctx, int32_t id);

/* ---- drags ---- */

/** ApplyDelta::update on the open core.object.dragDelta: the total delta since it began.
 */
Status updateDrag(Context &ctx, const TransformDelta &delta);
/**
 * The same with a delta for each of its objects, 16 numbers each: kind, basis (x, y, z,
 * w), pivot (3), vector (3), axis (3), angle, and one that is ignored.
 */
Status updateDragEach(Context &ctx, const litestl::util::Vector<double> &numbers);
/** ElementsDelta::update on the open core.element.dragDelta. */
Status updateElementsDrag(Context &ctx, const TransformDelta &delta);
/** How many objects the open drag moves; 0 for none. */
IntResult dragCount(Context &ctx);

/** Fills `out` with the list's bytes from `offset`: the copy for hosts that can't view
 * the module's memory. OutOfRange past the end. */
Status drawListRead(const DrawList &list, int64_t offset, std::span<uint8_t> out);

/** Adds the tool types and facades to the bindings (manager.cc). */
void addToolBindings(litestl::binding::BindingManager *mgr);

} // namespace coredcc::script

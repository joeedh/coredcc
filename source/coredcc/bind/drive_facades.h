#pragma once

/** \file
 * Graphs that drive objects, for scripts (drive/drive.h). The rules of facades.h hold
 * here too. Listing a graph in a scene is the command `core.scene.addGraph`.
 */

#include "coredcc/bind/facades.h"

namespace coredcc::script {

/** Gives the Context's Library a SceneDrivers, kept by the Runtime. AlreadyExists if it
 * has one, or another OverrideProvider. */
Status enableDrivers(Context &ctx);
/** Destroys the Library's SceneDrivers; nothing if it has none. Hosts call it before the
 * Library goes (a TS Session does). */
Status disableDrivers(Context &ctx);
/** What can't work in the listed graphs, as JSON: an array of objects with `graph` (a
 * block id as text), `node` (a node path, "" for the graph) and `message`. Brings the
 * drivers up to date at the scene's time first. FailedPrecondition without drivers. */
StringResult driverDiagnostics(Context &ctx);

} // namespace coredcc::script

#pragma once

/** \file
 * The BindingManager scripts load: every bound coredcc type and facade.
 */

namespace litestl::binding {
struct BindingManager;
}

namespace coredcc::script {

/**
 * Built on first use and never changed after: write-once static data, like the
 * binding descriptors it collects, not a registry (registries live in a Runtime).
 */
litestl::binding::BindingManager *bindings();

/**
 * Adds every bound coredcc type and facade to `mgr`, for a host that binds its own
 * types beside them in one manager (the TS runtime loads one). The host calls
 * `mgr->validate()` when it has added its own.
 */
void addBindings(litestl::binding::BindingManager *mgr);

} // namespace coredcc::script

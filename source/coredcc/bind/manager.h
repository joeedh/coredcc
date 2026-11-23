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

} // namespace coredcc::script

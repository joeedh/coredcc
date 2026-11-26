#include "coredcc/scene/overrides.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/lib/pin.h"
#include "coredcc/scene/transform_registry.h"

namespace coredcc {

namespace {

/* By Library. A Runtime extension. */
struct OverrideProviders {
  static constexpr const char *kExtName = "coredcc.OverrideProviders";
  explicit OverrideProviders(Runtime *)
  {
  }

  struct Entry {
    LibraryRef lib;
    OverrideProvider *provider = nullptr;
  };
  litestl::util::Vector<Entry> entries;
};

} // namespace

OverrideProvider *detail::overrideProvider(const Library &lib)
{
  for (const OverrideProviders::Entry &e : lib.runtime().ext<OverrideProviders>().entries)
  {
    if (e.lib.get() == &lib) {
      return e.provider;
    }
  }
  return nullptr;
}

Status setOverrideProvider(Library &lib, OverrideProvider &provider)
{
  Runtime &rt = lib.runtime();
  CDCC_TRY(rt.busyIfEvaluating("setOverrideProvider"));
  if (detail::overrideProvider(lib)) {
    return Status::error(StatusCode::AlreadyExists,
                         "setOverrideProvider: the Library has a provider");
  }
  litestl::util::Vector<OverrideProviders::Entry> &entries =
      rt.ext<OverrideProviders>().entries;
  for (int i = int(entries.size()) - 1; i >= 0; i--) {
    if (!entries[i].lib.get()) {
      entries.remove_at(i); /* its Library is gone */
    }
  }
  entries.append(OverrideProviders::Entry{lib.ref(), &provider});
  rt.ext<ChangeBus>().notifyAll();
  return Status();
}

void clearOverrideProvider(Library &lib, OverrideProvider &provider)
{
  Runtime &rt = lib.runtime();
  litestl::util::Vector<OverrideProviders::Entry> &entries =
      rt.ext<OverrideProviders>().entries;
  for (int i = int(entries.size()) - 1; i >= 0; i--) {
    if (entries[i].lib.get() == &lib && entries[i].provider == &provider) {
      entries.remove_at(i);
      rt.ext<ChangeBus>().notifyAll();
    }
  }
}

void detail::prepareOverrides(const Library &lib,
                               double time,
                               OverrideProvider::Driven &driven)
{
  driven = OverrideProvider::Driven();
  if (OverrideProvider *provider = overrideProvider(lib)) {
    (void)provider->prepare(lib, time, driven);
    if (lib.runtime().hasStickyError()) {
      (void)lib.runtime().takeStickyError();
    }
  }
}

Status detail::overridesOf(OverrideProvider *provider,
                           const Library &lib,
                           BlockHandle object,
                           const TransformType *type,
                           double time,
                           ChannelOverrides &out)
{
  out.reset(type ? type->channelCount() : 0);
  if (!provider || !type) {
    return Status();
  }
  Status s = provider->overridesOf(lib, object, *type, time, out);
  if (lib.runtime().hasStickyError()) {
    s = lib.runtime().takeStickyError();
  }
  return s;
}

Status detail::overridesOf(const Library &lib,
                           BlockHandle object,
                           const TransformType *type,
                           double time,
                           ChannelOverrides &out)
{
  return overridesOf(overrideProvider(lib), lib, object, type, time, out);
}

} // namespace coredcc

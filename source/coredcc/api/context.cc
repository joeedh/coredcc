#include "coredcc/api/context.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/api/format.h"
#include "coredcc/lib/library.h"
#include "coredcc/lib/provider.h"

namespace coredcc {

void Context::setScene(BlockHandle scene)
{
  if (scene == scene_) {
    return;
  }
  scene_ = scene;
  rt_.ext<ChangeBus>().bumpEpoch();
}

UnitSettings Context::units()
{
  const Datablock *b = lib_ ? lib_->get(scene_) : nullptr;
  void *data = b ? lib_->data(scene_) : nullptr; /* null unless it is Live */
  const BlockTypeRegistry::Entry *e =
      data ? rt_.ext<BlockTypeRegistry>().find(b->type()) : nullptr;
  if (!e) {
    return UnitSettings();
  }
  const ApiRegistry &reg = rt_.ext<ApiRegistry>();
  const ApiMember *m = reg.findMember(e->info.dataStruct, kUnitsMember);
  if (!m || m->kind() != MemberKind::Pointer) {
    return UnitSettings();
  }
  const PointerRef p = m->pointer().get(
      PointerRef{data, e->info.dataStruct, OwnerKey::ofBlock(b->id())}, *this);
  const bool settings = p.data && reg.isA(p.type ? p.type : m->pointer().target,
                                          StructId::of(kUnitSettingsStruct));
  return settings ? *static_cast<const UnitSettings *>(p.data) : UnitSettings();
}

} // namespace coredcc

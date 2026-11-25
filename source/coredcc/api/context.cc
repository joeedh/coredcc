#include "coredcc/api/context.h"

#include "coredcc/api/change_bus.h"

namespace coredcc {

void Context::setScene(BlockHandle scene)
{
  if (scene == scene_) {
    return;
  }
  scene_ = scene;
  rt_.ext<ChangeBus>().bumpEpoch();
}

} // namespace coredcc

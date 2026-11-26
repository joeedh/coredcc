#include "coredcc/drive/commands.h"
#include "coredcc/drive/drive.h"
#include "coredcc/drive/driver_list.h"
#include "coredcc/drive/node_types.h"

namespace coredcc {

Status registerDriveTypes(Runtime &rt)
{
  if (!rt.ext<NodeTypeRegistry>().find(drive::kTimeNode)) {
    CDCC_TRY(drive::registerNodeTypes(rt));
  }
  CDCC_TRY(detail::registerDriverListType(rt));
  return drive::registerCommands(rt);
}

} // namespace coredcc

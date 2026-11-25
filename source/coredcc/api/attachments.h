#pragma once

#include "coredcc/api/struct.h"

namespace coredcc {

/**
 * Adds an `attachments` collection to a block's struct, keyed by the attachment type's
 * name: `objects["A"].attachments["acme.notes"].text`. Lists the attachments whose type
 * has a data struct. The struct must be a block's root.
 */
void addAttachments(ApiStruct &st);

} // namespace coredcc

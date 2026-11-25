#pragma once

/* The test app's block type, `testapp.Item`, with a payload, and its struct. */
#include "coredcc/api/struct.h"
#include "coredcc/lib/library.h"

namespace testapp {

struct Item {
  double weight = 1;
  coredcc::RefCell other{coredcc::RefKind::Weak};
};

inline const coredcc::StructId kItem = coredcc::StructId::of("TestItem");

/** Registers the block type and the struct (`name`, `weight`, `other`). */
void registerItems(coredcc::Runtime &rt);
/** The items "Cube" and "Ball", as roots, with the LocalIds 1 and 2. */
void addItems(coredcc::Library &lib);
/** The `items` collection, by name, and `activeItem`, a pointer held in `cell`. */
void addItemMembers(coredcc::ApiStruct &root, coredcc::RefCell &cell);

} // namespace testapp

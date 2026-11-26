#pragma once

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <cstddef>
#include <initializer_list>

namespace coredcc::detail {

struct EnumBindingItem {
  const char *name;
  int value;
};

/** A script enum named `name` (as "coredcc::DepthMode"), of `size` bytes. A Binder keeps
 * the result in a static: it is made once and never freed. */
inline const litestl::binding::BindingBase *
bindEnum(const char *name, size_t size, std::initializer_list<EnumBindingItem> items)
{
  litestl::alloc::PermanentGuard guard;
  auto *e = new litestl::binding::types::Enum(name, size);
  for (const EnumBindingItem &item : items) {
    e->addItem(item.name, item.value);
  }
  return e;
}

} // namespace coredcc::detail

#include "coredcc/base/name_table.h"

#include "coredcc/base/text.h"

namespace coredcc {

bool NameTable::add(IdDomain domain, std::string_view name)
{
  return debugAddAs(domain, hashId(domain, name), name);
}

bool NameTable::debugAddAs(IdDomain domain, uint64_t id, std::string_view name)
{
  if (id == 0 || name.empty() || name.find('\0') != std::string_view::npos) {
    return false;
  }
  if (const char *existing = nameOf(domain, id)) {
    return name == existing;
  }
  const size_t cost = kEntryBytes + name.size();
  if (cost > kMaxBytes - bytes_) {
    return false;
  }
  bytes_ += cost;
  names_[int(domain)].add(id, lsString(name));
  return true;
}

const char *NameTable::nameOf(IdDomain domain, uint64_t id) const
{
  const litestl::util::string *s = names_[int(domain)].lookup_ptr(id);
  return s ? s->c_str() : nullptr;
}

} // namespace coredcc

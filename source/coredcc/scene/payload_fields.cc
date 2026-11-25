#include "coredcc/scene/payload_fields.h"

#include <cstring>

namespace coredcc::detail {

using litestl::util::Vector;

BlockEntry valueField(const char *name, PropValue v)
{
  BlockEntry e;
  e.name = name;
  e.value = std::move(v);
  return e;
}

BlockEntry refField(const char *name, std::span<const RefCell> cells)
{
  BlockEntry e;
  e.name = name;
  for (const RefCell &c : cells) {
    e.refs.append(c);
  }
  return e;
}

static bool isNamed(const BlockEntry &e, const char *name)
{
  return std::strcmp(e.name.c_str(), name) == 0;
}

bool isValueField(const BlockEntry &e, const char *name, PropValue::Type type)
{
  return isNamed(e, name) && e.unknownTag == 0 && e.refs.size() == 0 &&
         e.value.type() == type;
}

bool isRefField(const BlockEntry &e, const char *name)
{
  return isNamed(e, name) && e.unknownTag == 0 && e.value.isNone();
}

bool takeRef(const BlockEntry &e, const char *name, RefCell &cell)
{
  if (!isRefField(e, name) || e.refs.size() != 1) {
    return false;
  }
  cell.target = e.refs[0].target;
  return true;
}

bool hasField(const Vector<BlockEntry> &entries, const char *name)
{
  for (const BlockEntry &e : entries) {
    if (isNamed(e, name)) {
      return true;
    }
  }
  return false;
}

uint64_t fieldsMemSize(const Vector<BlockEntry> &entries)
{
  uint64_t n = 0;
  for (const BlockEntry &e : entries) {
    n += sizeof(BlockEntry) + e.raw.size() + e.refs.size() * sizeof(RefCell);
  }
  return n;
}

} // namespace coredcc::detail

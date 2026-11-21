#include "coredcc/props/ref.h"

#include "coredcc/base/id.h"
#include "coredcc/base/text.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace coredcc {

bool OwnerKey::operator==(const OwnerKey &b) const
{
  if (kind != b.kind) {
    return false;
  }
  switch (kind) {
    case Kind::None:
      return true;
    case Kind::Block:
      return block == b.block;
    case Kind::Runtime:
      return nameHash == b.nameHash;
  }
  return false;
}

uint64_t OwnerKey::computeHash() const
{
  switch (kind) {
    case Kind::None:
      return 0;
    case Kind::Block:
      return block.computeHash();
    case Kind::Runtime:
      return nameHash ^ 0x5bd1e9955bd1e995ull;
  }
  return 0;
}

KeySegment KeySegment::member(std::string_view name)
{
  KeySegment s;
  s.kind = Kind::Member;
  s.text = litestl::util::string(std::string(name).c_str());
  return s;
}

KeySegment KeySegment::stringKey(std::string_view key)
{
  KeySegment s;
  s.kind = Kind::StringKey;
  s.text = litestl::util::string(std::string(key).c_str());
  return s;
}

KeySegment KeySegment::ofHandle(int64_t handle)
{
  KeySegment s;
  s.kind = Kind::Handle;
  s.handle = handle;
  return s;
}

bool KeySegment::operator==(const KeySegment &b) const
{
  if (kind != b.kind) {
    return false;
  }
  return kind == Kind::Handle ? handle == b.handle : strcmp(text.c_str(), b.text.c_str()) == 0;
}

bool StableRef::operator==(const StableRef &b) const
{
  if (!(owner == b.owner) || key.size() != b.key.size()) {
    return false;
  }
  for (int i = 0; i < int(key.size()); i++) {
    if (!(key[i] == b.key[i])) {
      return false;
    }
  }
  return true;
}

uint64_t StableRef::computeHash() const
{
  uint64_t h = owner.computeHash();
  for (const KeySegment &s : key) {
    h = detail::fnv1a64(std::string_view(reinterpret_cast<const char *>(&s.kind), 1), h);
    if (s.kind == KeySegment::Kind::Handle) {
      h = detail::fnv1a64(
          std::string_view(reinterpret_cast<const char *>(&s.handle), sizeof(s.handle)), h);
    }
    else {
      h = detail::fnv1a64(s.text.c_str(), h);
    }
  }
  return h;
}

std::string StableRef::toString() const
{
  char buf[96];
  switch (owner.kind) {
    case OwnerKey::Kind::None:
      return "{null}";
    case OwnerKey::Kind::Block:
      snprintf(buf,
               sizeof(buf),
               "{block %016" PRIx64 ":%016" PRIx64 "%016" PRIx64 "}",
               owner.block.client.value,
               owner.block.local.hi,
               owner.block.local.lo);
      break;
    case OwnerKey::Kind::Runtime:
      snprintf(buf, sizeof(buf), "{runtime %016" PRIx64 "}", owner.nameHash);
      break;
  }
  std::string out = buf;
  for (const KeySegment &s : key) {
    switch (s.kind) {
      case KeySegment::Kind::Member:
        out += '.';
        out += s.text.c_str();
        break;
      case KeySegment::Kind::StringKey:
        out += '[';
        appendQuoted(out, s.text.c_str());
        out += ']';
        break;
      case KeySegment::Kind::Handle:
        out += "[#" + std::to_string(s.handle) + "]";
        break;
    }
  }
  return out;
}

} // namespace coredcc

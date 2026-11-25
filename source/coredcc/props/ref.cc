#include "coredcc/props/ref.h"

#include "coredcc/base/assert.h"
#include "coredcc/base/id.h"
#include "coredcc/base/text.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace coredcc {

bool OwnerKey::operator==(const OwnerKey &b) const
{
  if (isNull() || b.isNull()) {
    return isNull() && b.isNull();
  }
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
  if (isNull()) {
    return 0;
  }
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
  s.text = lsString(name);
  return s;
}

KeySegment KeySegment::stringKey(std::string_view key)
{
  KeySegment s;
  s.kind = Kind::StringKey;
  s.text = lsString(key);
  return s;
}

KeySegment KeySegment::ofHandle(int64_t handle)
{
  CDCC_CHECK(handle >= 0, "handles are non-negative (the path grammar has no '#-')");
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
  return kind == Kind::Handle ? handle == b.handle
                              : strcmp(text.c_str(), b.text.c_str()) == 0;
}

bool StableRef::operator==(const StableRef &b) const
{
  if (isNull() || b.isNull()) {
    return isNull() && b.isNull();
  }
  if (!(owner == b.owner) || type != b.type || key.size() != b.key.size()) {
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
  if (isNull()) {
    return 0;
  }
  uint64_t h = owner.computeHash() ^ type.value * 0x9E3779B97F4A7C15ull;
  auto bytes = [&h](const void *p, size_t n) {
    h = detail::fnv1a64(std::string_view(static_cast<const char *>(p), n), h);
  };
  for (const KeySegment &s : key) {
    bytes(&s.kind, 1);
    if (s.kind == KeySegment::Kind::Handle) {
      bytes(&s.handle, sizeof(s.handle));
    } else {
      /* Length first, so a key holding a kind byte can't mimic two segments. */
      const uint64_t n = strlen(s.text.c_str());
      bytes(&n, sizeof(n));
      bytes(s.text.c_str(), size_t(n));
    }
  }
  return h;
}

std::string StableRef::toString() const
{
  char buf[96];
  if (isNull()) {
    return "{null}";
  }
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

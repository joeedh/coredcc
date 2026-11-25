#include "coredcc/graph/interior.h"

#include "coredcc/base/text.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace coredcc {

/* An id in its only spelling. */
static std::optional<NodeId> parseId(std::string_view text)
{
  int64_t id = 0;
  const char *end = text.data() + text.size();
  const std::from_chars_result r = std::from_chars(text.data(), end, id);
  if (r.ec != std::errc() || r.ptr != end || id <= 0 || text[0] == '0') {
    return std::nullopt;
  }
  return NodeId{id};
}

std::optional<InteriorKey> InteriorKey::parse(std::string_view text)
{
  const size_t dot = text.find('.');
  if (!isInteriorKey(text) || dot == std::string_view::npos) {
    return std::nullopt;
  }
  InteriorKey k;
  k.key = lsString(text.substr(dot + 1));
  if (k.key.size() == 0 || isInteriorKey(k.key.c_str())) {
    return std::nullopt;
  }
  std::string_view path = text.substr(1, dot - 1);
  while (true) {
    const size_t slash = path.find('/');
    const std::optional<NodeId> id = parseId(path.substr(0, slash));
    if (!id) {
      return std::nullopt;
    }
    if (slash == std::string_view::npos) {
      k.node = *id;
      return k;
    }
    k.groups.append(*id);
    path.remove_prefix(slash + 1);
  }
}

std::string InteriorKey::toString() const
{
  std::string s = "@";
  for (NodeId id : groups) {
    s += std::to_string(id.value) + "/";
  }
  return s + std::to_string(node.value) + "." + key.c_str();
}

InteriorKey InteriorKey::inner() const
{
  InteriorKey k = *this;
  k.groups.remove_at(0);
  return k;
}

bool InteriorKey::operator==(const InteriorKey &b) const
{
  return node == b.node && strcmp(key.c_str(), b.key.c_str()) == 0 &&
         groups.size() == b.groups.size() &&
         std::equal(groups.begin(), groups.end(), b.groups.begin());
}

std::optional<SocketInfo> findInterior(const GraphDef &def,
                                       const GraphNode &group,
                                       SocketSide side,
                                       const InteriorKey &key)
{
  const GraphDef *d = &def;
  const GraphNode *n = &group;
  for (int i = 0; i <= int(key.groups.size()); i++) {
    if (!n || !n->type() || n->type()->role != NodeRole::Group) {
      return std::nullopt;
    }
    d = d->groupTarget(*n);
    if (!d) {
      return std::nullopt;
    }
    n = d->node(i < int(key.groups.size()) ? key.groups[i] : key.node);
  }
  if (n) {
    for (const SocketInfo &s : d->sockets(*n, side)) {
      if (strcmp(s.key.c_str(), key.key.c_str()) == 0) {
        return s;
      }
    }
  }
  return std::nullopt;
}

} // namespace coredcc

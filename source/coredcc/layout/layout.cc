#include "coredcc/layout/layout.h"

#include "coredcc/command/last_used.h"
#include "coredcc/props/value.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace coredcc {

bool valueHolds(const PropValue &value)
{
  switch (value.type()) {
  case PropValue::Type::Bool:
    return value.asBool();
  case PropValue::Type::Int:
    return value.asInt() != 0;
  case PropValue::Type::String:
    return value.asString().size() > 0;
  case PropValue::Type::Pointer:
    return !value.asPointer().isNull();
  default:
    return false;
  }
}

bool conditionHolds(const LayoutCondition &condition, const Result<PropValue> &value)
{
  return value.ok() && valueHolds(*value) != condition.negated;
}

bool containerIsThere(const Result<PropValue> &value)
{
  if (!value.ok()) {
    return value.status().code() == StatusCode::Unsupported;
  }
  return value->type() != PropValue::Type::Pointer || !value->asPointer().isNull();
}

Layout::Layout()
{
  nodes_.append(LayoutNode());
  paths_.append(Path::root());
}

const LayoutAttr *Layout::attr(int node, LayoutAttrId id) const
{
  const LayoutNode &n = nodes_[node];
  for (uint32_t i = 0; i < n.attrCount; i++) {
    const LayoutAttr &a = attrs_[n.attrBegin + i];
    if (a.id == id) {
      return &a;
    }
  }
  return nullptr;
}

const char *Layout::text(int node, LayoutAttrId id) const
{
  const LayoutAttr *a = attr(node, id);
  return a ? a->value.c_str() : "";
}

bool Layout::flag(int node, LayoutAttrId id, bool byDefault) const
{
  const LayoutAttr *a = attr(node, id);
  return a ? strcmp(a->value.c_str(), "true") == 0 : byDefault;
}

const LayoutAttr *Layout::freeAttr(int node, std::string_view name) const
{
  const LayoutNode &n = nodes_[node];
  for (uint32_t i = 0; i < n.attrCount; i++) {
    const LayoutAttr &a = attrs_[n.attrBegin + i];
    if (a.id == LayoutAttrId::Free && a.name.c_str() == name) {
      return &a;
    }
  }
  return nullptr;
}

int Layout::findId(std::string_view id) const
{
  const int32_t *end = byId_.data() + byId_.size();
  const int32_t *at =
      std::lower_bound(byId_.data(), end, id, [this](int32_t i, std::string_view id) {
        return text(i, LayoutAttrId::Id) < id;
      });
  return at != end && text(*at, LayoutAttrId::Id) == id ? *at : -1;
}

int Layout::version() const
{
  const LayoutAttr *a = attr(0, LayoutAttrId::Version);
  return a ? atoi(a->value.c_str()) : 1;
}

const Path &Layout::prefixOf(int node) const
{
  const int parent = nodes_[node].parent;
  return parent < 0 ? paths_[0] : effectivePath(parent);
}

const Path &Layout::effectivePath(int node) const
{
  return paths_[nodes_[node].effectivePath];
}

Path Layout::optionsPath(int node) const
{
  Path out = Path::root();
  PathSegment s;
  s.text = LastUsedStore::kMember;
  out.segments.append(s);
  s.kind = SegmentKind::StringKey;
  s.text = text(node, LayoutAttrId::Target);
  out.segments.append(std::move(s));
  return out;
}

bool Layout::condition(int node, LayoutAttrId id, LayoutCondition &out) const
{
  const LayoutAttr *a = attr(node, id);
  if (!a) {
    return false;
  }
  out.negated = a->value.c_str()[0] == '!';
  out.path = paths_[a->joined];
  return true;
}

bool Layout::operator==(const Layout &b) const
{
  if (nodes_.size() != b.nodes_.size() || attrs_.size() != b.attrs_.size()) {
    return false;
  }
  for (int i = 0; i < nodeCount(); i++) {
    const LayoutNode &x = nodes_[i], &y = b.nodes_[i];
    if (x.kind != y.kind || x.parent != y.parent || x.attrBegin != y.attrBegin) {
      return false;
    }
  }
  for (int i = 0; i < int(attrs_.size()); i++) {
    const LayoutAttr &x = attrs_[i], &y = b.attrs_[i];
    if (x.id != y.id || strcmp(x.name.c_str(), y.name.c_str()) != 0 ||
        strcmp(x.value.c_str(), y.value.c_str()) != 0)
    {
      return false;
    }
  }
  return true;
}

Result<Path>
joinLayoutPath(const Path &prefix, std::string_view path, size_t *errorOffset)
{
  Result<Path> own = parsePath(path, errorOffset);
  if (!own.ok() || own->fromRoot) {
    return own;
  }
  Path out = prefix;
  for (PathSegment &s : own->segments) {
    out.segments.append(std::move(s));
  }
  return out;
}

} // namespace coredcc

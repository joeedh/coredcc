#include "coredcc/layout/layout.h"

#include "coredcc/base/assert.h"

#include <cstring>
#include <utility>

namespace coredcc {

namespace {

/* Joins text that LayoutAssembler checked. */
void join(Path &to, std::string_view path)
{
  if (path[0] == '/') {
    to.segments.clear();
    path.remove_prefix(1);
  }
  if (path.empty()) {
    return;
  }
  Result<Path> own = parsePath(path);
  CDCC_CHECK(own.ok(), "a Layout holds a path that isn't one");
  for (PathSegment &s : own->segments) {
    to.segments.append(std::move(s));
  }
}

} // namespace

Layout::Layout()
{
  nodes_.append(LayoutNode());
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
  for (int i = 0; i < nodeCount(); i++) {
    const LayoutAttr *a = attr(i, LayoutAttrId::Id);
    if (a && a->value.c_str() == id) {
      return i;
    }
  }
  return -1;
}

Path Layout::prefixOf(int node) const
{
  const int parent = nodes_[node].parent;
  return parent < 0 ? Path() : effectivePath(parent);
}

Path Layout::effectivePath(int node) const
{
  /* From the innermost: a path from the root hides those around it. */
  const LayoutAttr *chain[LayoutLimits::kMaxDepth];
  int count = 0;
  for (int i = node; i >= 0; i = nodes_[i].parent) {
    if (const LayoutAttr *a = attr(i, LayoutAttrId::Path)) {
      chain[count++] = a;
      if (a->value.c_str()[0] == '/') {
        break;
      }
    }
  }
  Path out;
  while (count > 0) {
    join(out, chain[--count]->value.c_str());
  }
  return out;
}

bool Layout::condition(int node, LayoutAttrId id, LayoutCondition &out) const
{
  const LayoutAttr *a = attr(node, id);
  if (!a) {
    return false;
  }
  const char *text = a->value.c_str();
  out.negated = text[0] == '!';
  out.path = prefixOf(node);
  join(out.path, text + (out.negated ? 1 : 0));
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

Result<Path> joinLayoutPath(const Path &prefix, std::string_view path, size_t *errorOffset)
{
  const bool fromRoot = !path.empty() && path[0] == '/';
  if (fromRoot && path.size() == 1) {
    return Path();
  }
  Result<Path> own = parsePath(path.substr(fromRoot ? 1 : 0), errorOffset);
  if (!own.ok()) {
    if (errorOffset && fromRoot) {
      ++*errorOffset;
    }
    return own.status();
  }
  if (fromRoot) {
    return own;
  }
  Path out = prefix;
  for (PathSegment &s : own->segments) {
    out.segments.append(std::move(s));
  }
  return out;
}

} // namespace coredcc

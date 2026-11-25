#include "coredcc/base/json_writer.h"
#include "coredcc/layout/draft.h"

#include <string>
#include <vector>

namespace coredcc {

namespace {

bool isBool(LayoutAttrId id)
{
  return id == LayoutAttrId::Closed || id == LayoutAttrId::Expand ||
         id == LayoutAttrId::IconsOnly || id == LayoutAttrId::ShowLabel;
}

std::string joined(const std::string &prefix, std::string_view path)
{
  return layout_detail::joinPathText(prefix, path).valueOr(std::string());
}

/* `around` is the prefix of its paths. Gives that of its children. */
std::string writeNode(JsonWriter &out, const Layout &layout, int i, const std::string &around)
{
  const LayoutNode &n = layout.node(i);
  std::string within = around;
  out.key("kind").string(layoutKindName(n.kind));
  bool free = false;
  for (uint32_t j = 0; j < n.attrCount; j++) {
    const LayoutAttr &a = layout.attrs()[n.attrBegin + j];
    const char *value = a.value.c_str();
    if (a.id == LayoutAttrId::Free) { /* they are the last ones */
      if (!free) {
        out.key("attrs").beginObject();
        free = true;
      }
      out.key(a.name.c_str()).string(value);
      continue;
    }
    out.key(layoutAttrName(a.id));
    if (isBool(a.id)) {
      out.boolean(layout.flag(i, a.id, false));
    }
    else if (a.id == LayoutAttrId::VisibleIf || a.id == LayoutAttrId::EnabledIf) {
      const bool negated = value[0] == '!';
      out.beginObject().key("path").string(joined(around, value + (negated ? 1 : 0)));
      if (negated) {
        out.key("not").boolean(true);
      }
      out.endObject();
    }
    else {
      out.string(value);
    }
    if (a.id == LayoutAttrId::Path) {
      within = joined(around, value);
      out.key("effectivePath").string(within);
    }
  }
  if (free) {
    out.endObject();
  }
  if ((n.kind == LayoutKind::Tool || n.kind == LayoutKind::ToolOptions) && !around.empty()) {
    out.key("prefix").string(around);
  }
  return within;
}

} // namespace

std::string layoutToJson(const Layout &layout)
{
  JsonWriter out;
  std::vector<std::string> prefixes(1); /* of the open nodes' children, the root's parent first */
  int i = 0;
  while (true) {
    out.beginObject();
    prefixes.push_back(writeNode(out, layout, i, prefixes.back()));
    if (const int child = layout.node(i).firstChild; child >= 0) {
      out.key("children").beginArray();
      i = child;
      continue;
    }
    out.endObject();
    prefixes.pop_back();
    while (layout.node(i).nextSibling < 0) {
      i = layout.node(i).parent;
      if (i < 0) {
        return out.take();
      }
      out.endArray().endObject();
      prefixes.pop_back();
    }
    i = layout.node(i).nextSibling;
  }
}

} // namespace coredcc

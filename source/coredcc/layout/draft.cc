#include "coredcc/layout/draft.h"

#include "coredcc/base/text.h"
#include "coredcc/base/utf8.h"
#include "coredcc/parse/invocation.h"

#include <algorithm>
#include <set>
#include <string>

namespace coredcc {

namespace {

using Id = LayoutAttrId;
using Kind = LayoutKind;

enum class Form : uint8_t { Text, NonEmpty, Path, Condition, Bool, Invocation };

struct KindInfo {
  const char *name;
  bool container;
  /** The attribute it must have; Free for none. */
  Id required;
};

/* By LayoutKind. */
constexpr KindInfo kKinds[] = {
    {"layout", true, Id::Free},
    {"column", true, Id::Free},
    {"row", true, Id::Free},
    {"panel", true, Id::Free},
    {"tabs", true, Id::Free},
    {"tab", true, Id::Free},
    {"prop", false, Id::Path},
    {"tool", false, Id::Invocation},
    {"toolOptions", false, Id::Target},
    {"label", false, Id::Label},
    {"separator", false, Id::Free},
    {"custom", true, Id::Tag},
};

constexpr uint32_t bit(Kind k)
{
  return 1u << uint32_t(k);
}
template <class... K> constexpr uint32_t bits(K... k)
{
  return (bit(k) | ...);
}

constexpr uint32_t kAll = (1u << (uint32_t(Kind::Custom) + 1)) - 1;
constexpr uint32_t kTexts = bits(Kind::Panel, Kind::Tab, Kind::Prop, Kind::Tool,
                                 Kind::ToolOptions, Kind::Label, Kind::Custom);

struct AttrInfo {
  const char *name;
  Form form;
  uint32_t kinds;
};

/* By LayoutAttrId. */
constexpr AttrInfo kAttrs[] = {
    {"path", Form::Path,
     bits(Kind::Layout, Kind::Column, Kind::Row, Kind::Panel, Kind::Tabs, Kind::Tab, Kind::Prop,
          Kind::Custom)},
    {"label", Form::Text, kTexts},
    {"icon", Form::Text, kTexts},
    {"description", Form::Text, kTexts},
    {"id", Form::NonEmpty, kAll},
    {"class", Form::Text, kAll},
    {"visibleIf", Form::Condition, kAll & ~bit(Kind::Layout)},
    {"enabledIf", Form::Condition, kAll & ~bit(Kind::Layout)},
    {"invocation", Form::Invocation, bit(Kind::Tool)},
    {"target", Form::NonEmpty, bit(Kind::ToolOptions)},
    {"widget", Form::NonEmpty, bit(Kind::Prop)},
    {"closed", Form::Bool, bit(Kind::Panel)},
    {"expand", Form::Bool, bits(Kind::Column, Kind::Row, Kind::Prop)},
    {"iconsOnly", Form::Bool, bits(Kind::Prop, Kind::Tool)},
    {"showLabel", Form::Bool, bit(Kind::Prop)},
    {"tag", Form::NonEmpty, bit(Kind::Custom)},
    {"text", Form::Text, bit(Kind::Custom)},
    {"", Form::Text, bit(Kind::Custom)},
};
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == size_t(Kind::Custom) + 1);
static_assert(sizeof(kAttrs) / sizeof(kAttrs[0]) == size_t(Id::Free) + 1);

const KindInfo &info(Kind k)
{
  return kKinds[int(k)];
}

std::string element(Kind k)
{
  return std::string("<") + info(k).name + ">";
}

Status invalid(const std::string &what)
{
  return Status::error(StatusCode::InvalidArgument, what.c_str());
}

Status tooMany(const std::string &what)
{
  return Status::error(StatusCode::OutOfRange, what.c_str());
}

/* The checks of one layout. */
class Checker {
 public:
  Checker(std::vector<DraftNode> &nodes, DraftFault &fault) : nodes_(nodes), fault_(fault) {}

  bool run()
  {
    if (nodes_.size() > size_t(LayoutLimits::kMaxNodes)) {
      return fail(LayoutLimits::kMaxNodes, nodes_[LayoutLimits::kMaxNodes].offset,
                  tooMany("there are more than " + std::to_string(LayoutLimits::kMaxNodes) +
                          " elements"));
    }
    depth_.resize(nodes_.size());
    prefix_.resize(nodes_.size());
    for (int i = 0; i < int(nodes_.size()); i++) {
      if (!place(i) || !attributes(i) || !paths(i)) {
        return false;
      }
    }
    return true;
  }

 private:
  bool fail(int node, size_t offset, Status s)
  {
    fault_.node = node;
    fault_.offset = offset;
    fault_.status = std::move(s);
    return false;
  }
  /* About the value, at `inner` in it. */
  bool failIn(int node, const DraftAttr &a, size_t inner, const Status &s)
  {
    std::string what = (a.id == Id::Free ? a.name : layoutAttrName(a.id)) + ": ";
    /* The parsers of paths and invocations say it themselves. */
    what = std::string_view(s.message()).starts_with(what) ? s.message() : what + s.message();
    return fail(node, a.valueOffset + (a.verbatim ? inner : 0),
                Status::error(s.code(), what.c_str()));
  }

  bool place(int i)
  {
    const DraftNode &n = nodes_[i];
    if ((n.kind == Kind::Layout) != (i == 0)) {
      return fail(i, n.offset, invalid(i == 0 ? "the root isn't a <layout>" :
                                                "only the root is a <layout>"));
    }
    if (i == 0) {
      depth_[i] = 1;
      return true;
    }
    DraftNode &p = nodes_[n.parent];
    depth_[i] = depth_[n.parent] + 1;
    if (depth_[i] > LayoutLimits::kMaxDepth) {
      return fail(i, n.offset, tooMany("the elements are nested more than " +
                                       std::to_string(LayoutLimits::kMaxDepth) + " deep"));
    }
    if (!info(p.kind).container) {
      return fail(i, n.offset, invalid("a " + element(p.kind) + " can't hold elements"));
    }
    if ((p.kind == Kind::Tabs) != (n.kind == Kind::Tab)) {
      return fail(i, n.offset, invalid(n.kind == Kind::Tab ?
                                           "a <tab> must be in a <tabs>" :
                                           "a <tabs> holds only <tab> elements"));
    }
    if (p.find(Id::Text)) {
      return fail(i, n.offset, invalid("text and elements can't be mixed"));
    }
    return true;
  }

  bool attributes(int i)
  {
    DraftNode &n = nodes_[i];
    if (n.attrs.size() > size_t(LayoutLimits::kMaxAttrs)) {
      return fail(i, n.attrs[LayoutLimits::kMaxAttrs].offset,
                  tooMany("an element has more than " + std::to_string(LayoutLimits::kMaxAttrs) +
                          " attributes"));
    }
    std::sort(n.attrs.begin(), n.attrs.end(), [](const DraftAttr &a, const DraftAttr &b) {
      return a.id != b.id ? a.id < b.id : a.name < b.name;
    });
    for (const DraftAttr &a : n.attrs) {
      if (!layout_detail::mayHave(n.kind, a.id)) {
        return fail(i, a.offset,
                    invalid("a " + element(n.kind) + " has no '" + layoutAttrName(a.id) + "'"));
      }
      if (a.id == Id::Free && !layout_detail::isXmlName(a.name)) {
        return fail(i, a.offset, invalid("'" + a.name + "' isn't an attribute's name"));
      }
      if (!form(i, a)) {
        return false;
      }
    }
    if (const Id need = info(n.kind).required; need != Id::Free) {
      const DraftAttr *a = n.find(need);
      if (!a || a->value.empty()) {
        return fail(i, n.offset,
                    invalid("a " + element(n.kind) + " needs " +
                            (need == Id::Label ? "text" : "'" + std::string(layoutAttrName(need)) +
                                                              "'")));
      }
    }
    if (const DraftAttr *a = n.find(Id::Id)) {
      if (!ids_.insert(a->value).second) {
        return fail(i, a->offset, invalid("the id '" + a->value + "' is used twice"));
      }
    }
    return true;
  }

  bool form(int i, const DraftAttr &a)
  {
    const char *bad = nullptr;
    if (const size_t at = layout_detail::firstBadByte(a.value, bad); at < a.value.size()) {
      return failIn(i, a, at, invalid(bad));
    }
    const Form form = kAttrs[int(a.id)].form;
    if (form == Form::NonEmpty && a.value.empty()) {
      return failIn(i, a, 0, invalid("it is empty"));
    }
    if (form == Form::Bool && a.value != "true" && a.value != "false") {
      return failIn(i, a, 0, invalid("it is neither 'true' nor 'false'"));
    }
    if (form == Form::Invocation) {
      size_t at = 0;
      if (Result<Invocation> r = parseInvocation(a.value, &at); !r.ok()) {
        return failIn(i, a, at, r.status());
      }
    }
    return true;
  }

  std::string around(int i) const
  {
    const int parent = nodes_[i].parent;
    return parent < 0 ? "/" : prefix_[parent];
  }

  /* The path in the value from `skip` on, joined with the prefix around node i, as text. */
  bool join(int i, const DraftAttr &a, size_t skip, std::string &out)
  {
    size_t at = 0;
    Result<std::string> p =
        layout_detail::joinPathText(around(i),
                                    std::string_view(a.value).substr(skip), &at);
    if (!p.ok()) {
      return failIn(i, a, skip + at, p.status());
    }
    out = std::move(*p);
    if (out.size() > LayoutLimits::kMaxPathBytes) {
      return failIn(i, a, 0, tooMany("it is longer than " +
                                     std::to_string(LayoutLimits::kMaxPathBytes) +
                                     " bytes, with the prefixes around it"));
    }
    return true;
  }

  bool paths(int i)
  {
    prefix_[i] = around(i);
    for (const DraftAttr &a : nodes_[i].attrs) {
      const Form form = kAttrs[int(a.id)].form;
      std::string condition;
      if (form == Form::Path && !join(i, a, 0, prefix_[i])) {
        return false;
      }
      if (form == Form::Condition &&
          !join(i, a, !a.value.empty() && a.value[0] == '!' ? 1 : 0, condition))
      {
        return false;
      }
    }
    return true;
  }

  std::vector<DraftNode> &nodes_;
  DraftFault &fault_;
  std::vector<int> depth_;
  /** Joined, for the paths of a node's children. */
  std::vector<std::string> prefix_;
  std::set<std::string> ids_;
};

} // namespace

const char *layoutKindName(LayoutKind kind)
{
  return info(kind).name;
}

const char *layoutAttrName(LayoutAttrId id)
{
  return kAttrs[int(id)].name;
}

DraftAttr *DraftNode::find(LayoutAttrId id, std::string_view name)
{
  for (DraftAttr &a : attrs) {
    if (a.id == id && a.name == name) {
      return &a;
    }
  }
  return nullptr;
}

namespace layout_detail {

bool isKindName(std::string_view name, LayoutKind &out)
{
  if (name == "sep") { /* path.ux's */
    out = Kind::Separator;
    return true;
  }
  for (int i = 0; i <= int(Kind::Custom); i++) {
    if (name == kKinds[i].name) {
      out = Kind(i);
      return true;
    }
  }
  return false;
}

std::vector<std::string_view> kindNames()
{
  std::vector<std::string_view> out;
  for (const KindInfo &k : kKinds) {
    out.push_back(k.name);
  }
  return out;
}

bool mayHave(LayoutKind kind, LayoutAttrId id)
{
  return (kAttrs[int(id)].kinds & bit(kind)) != 0;
}

bool isContainer(LayoutKind kind)
{
  return info(kind).container;
}

Result<LayoutAttrId> attrOf(LayoutKind kind, std::string_view name)
{
  Suggester suggest(name);
  bool known = false;
  for (int i = 0; i < int(Id::Text); i++) {
    const bool may = mayHave(kind, Id(i));
    if (name == kAttrs[i].name) {
      if (may) {
        return Id(i);
      }
      known = true;
    }
    if (may) {
      suggest.consider(kAttrs[i].name);
    }
  }
  if (kind == Kind::Custom) {
    return Id::Free;
  }
  Status s = invalid(known ? "a " + element(kind) + " has no '" + std::string(name) + "'" :
                             "there is no attribute '" + std::string(name) + "'");
  suggest.addTo(s);
  return s;
}

bool isXmlNameChar(char c, bool first)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         (!first && ((c >= '0' && c <= '9') || c == '.' || c == '-'));
}

bool isXmlName(std::string_view s)
{
  for (size_t i = 0; i < s.size(); i++) {
    if (!isXmlNameChar(s[i], i == 0)) {
      return false;
    }
  }
  return !s.empty();
}

size_t firstBadByte(std::string_view s, const char *&what)
{
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i]);
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
      what = c == 0 ? "a NUL character" : "a control character";
      return i;
    }
    const size_t n = utf8SequenceLength(s, i);
    if (n == 0) {
      what = "the text isn't valid UTF-8";
      return i;
    }
    if (c == 0xEF && uint8_t(s[i + 1]) == 0xBF && uint8_t(s[i + 2]) >= 0xBE) {
      what = "U+FFFE and U+FFFF aren't characters";
      return i;
    }
    i += n;
  }
  return s.size();
}

Result<std::string> joinPathText(const std::string &prefix,
                                 std::string_view path,
                                 size_t *errorOffset)
{
  Result<Path> own = joinLayoutPath(Path(), path, errorOffset);
  if (!own.ok()) {
    return own.status();
  }
  return own->fromRoot ? own->toString() : prefix + (prefix == "/" ? "" : ".") + own->toString();
}

} // namespace layout_detail

Result<Layout> LayoutAssembler::assemble(std::vector<DraftNode> nodes, DraftFault &fault)
{
  if (!Checker(nodes, fault).run()) {
    return fault.status;
  }
  const int count = int(nodes.size());
  std::vector<int> first(count, -1), last(count, -1), next(count, -1), place(count, -1);
  for (int i = 1; i < count; i++) {
    const int p = nodes[i].parent;
    (last[p] < 0 ? first[p] : next[last[p]]) = i;
    last[p] = i;
  }
  /* Preorder. */
  Layout out;
  out.nodes_.clear();
  int i = 0;
  while (i >= 0) {
    const DraftNode &from = nodes[i];
    place[i] = int(out.nodes_.size());
    LayoutNode n;
    n.kind = from.kind;
    n.parent = from.parent < 0 ? -1 : place[from.parent];
    n.attrBegin = uint32_t(out.attrs_.size());
    n.attrCount = uint32_t(from.attrs.size());
    n.offset = uint32_t(from.offset);
    out.nodes_.append(n);
    for (const DraftAttr &a : from.attrs) {
      LayoutAttr to;
      to.id = a.id;
      to.name = lsString(a.name);
      to.value = lsString(a.value);
      to.offset = uint32_t(a.offset);
      out.attrs_.append(std::move(to));
    }
    if (first[i] >= 0) {
      i = first[i];
      continue;
    }
    while (i >= 0 && next[i] < 0) {
      i = nodes[i].parent;
    }
    i = i < 0 ? -1 : next[i];
  }
  for (int j = 0; j < count; j++) {
    LayoutNode &n = out.nodes_[place[j]];
    n.firstChild = first[j] < 0 ? -1 : place[first[j]];
    n.nextSibling = next[j] < 0 ? -1 : place[next[j]];
  }
  return out;
}

} // namespace coredcc

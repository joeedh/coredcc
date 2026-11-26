#include "coredcc/layout/builder.h"

#include "coredcc/base/assert.h"
#include "coredcc/layout/draft.h"

#include <string>
#include <utility>
#include <vector>

namespace coredcc {

struct LayoutBuilder::State {
  std::vector<DraftNode> nodes = std::vector<DraftNode>(1);
  size_t bytes = 0;
  /** The first mistake that the nodes don't show. */
  Status error;

  void fail(int node, StatusCode code, const std::string &what)
  {
    if (error.ok()) {
      error = Status::error(code, message(node, what).c_str());
    }
  }
  std::string message(int node, const std::string &what) const
  {
    return "layout: node " + std::to_string(node) + " (" +
           layoutKindName(nodes[node].kind) + "): " + what;
  }
};

LayoutBuilder::LayoutBuilder() : s_(std::make_unique<State>())
{
}
LayoutBuilder::LayoutBuilder(const Layout &layout) : LayoutBuilder()
{
  s_->nodes.clear();
  for (int i = 0; i < layout.nodeCount(); i++) {
    const LayoutNode &n = layout.node(i);
    DraftNode to;
    to.kind = n.kind;
    to.parent = n.parent;
    for (uint32_t j = 0; j < n.attrCount; j++) {
      const LayoutAttr &from = layout.attrs()[n.attrBegin + j];
      DraftAttr a;
      a.id = from.id;
      a.name = from.name.c_str();
      a.value = from.value.c_str();
      s_->bytes += a.name.size() + a.value.size();
      to.attrs.push_back(std::move(a));
    }
    s_->nodes.push_back(std::move(to));
  }
}

LayoutBuilder::~LayoutBuilder() = default;

LayoutScope LayoutBuilder::at(int node)
{
  CDCC_CHECK(node >= 0 && node < int(s_->nodes.size()),
             "LayoutBuilder::at: no such node");
  return LayoutScope(*this, node);
}

int LayoutBuilder::findId(std::string_view id) const
{
  for (size_t i = 0; i < s_->nodes.size(); i++) {
    const DraftAttr *a = s_->nodes[i].find(LayoutAttrId::Id);
    if (a && a->value == id) {
      return int(i);
    }
  }
  return -1;
}

int LayoutBuilder::add(int parent, LayoutKind kind)
{
  std::vector<DraftNode> &nodes = s_->nodes;
  if (nodes.size() > size_t(LayoutLimits::kMaxNodes)) {
    return int(nodes.size()) - 1; /* one too many is enough to fail */
  }
  DraftNode n;
  n.kind = kind;
  n.parent = parent;
  nodes.push_back(std::move(n));
  return int(nodes.size()) - 1;
}

void LayoutBuilder::set(int node,
                        LayoutAttrId id,
                        std::string_view name,
                        std::string_view value)
{
  s_->bytes += name.size() + value.size();
  if (s_->bytes > LayoutLimits::kMaxBytes) {
    s_->fail(node, StatusCode::OutOfRange, "the attributes are more than 1 MiB of text");
    return;
  }
  DraftNode &n = s_->nodes[node];
  if (DraftAttr *a = n.find(id, name)) {
    a->value = value;
    return;
  }
  if (n.attrs.size() > size_t(LayoutLimits::kMaxAttrs)) {
    return;
  }
  DraftAttr a;
  a.id = id;
  a.name = name;
  a.value = value;
  n.attrs.push_back(std::move(a));
}

void LayoutBuilder::set(int node, std::string_view name, std::string_view value)
{
  Result<LayoutAttrId> id = layout_detail::attrOf(s_->nodes[node].kind, name);
  if (!id.ok()) {
    if (s_->error.ok()) {
      s_->fail(node, id.status().code(), id.status().message());
      for (const auto &suggestion : id.status().suggestions()) {
        s_->error.suggest(suggestion);
      }
    }
    return;
  }
  set(node, *id, *id == LayoutAttrId::Free ? name : std::string_view(), value);
}

Result<Layout> LayoutBuilder::finish()
{
  std::unique_ptr<State> s = std::exchange(s_, std::make_unique<State>());
  if (!s->error.ok()) {
    return s->error;
  }
  DraftFault fault;
  Result<Layout> out = LayoutAssembler::assemble(s->nodes, fault);
  if (!out.ok()) {
    return Status::error(fault.status.code(),
                         s->message(fault.node, fault.status.message()).c_str());
  }
  return out;
}

LayoutScope LayoutScope::add(LayoutKind kind)
{
  return LayoutScope(*b_, b_->add(index_, kind));
}

LayoutScope &LayoutScope::set(LayoutAttrId id, std::string_view value)
{
  if (id == LayoutAttrId::Free) {
    b_->s_->fail(
        index_, StatusCode::InvalidArgument, "a free attribute is set by its name");
    return *this;
  }
  b_->set(index_, id, {}, value);
  return *this;
}

LayoutScope &LayoutScope::setFlag(LayoutAttrId id, bool value)
{
  return set(id, std::string_view(value ? "true" : "false"));
}

LayoutScope &LayoutScope::set(std::string_view name, std::string_view value)
{
  b_->set(index_, name, value);
  return *this;
}

LayoutScope LayoutScope::column()
{
  return add(LayoutKind::Column);
}

LayoutScope LayoutScope::row()
{
  return add(LayoutKind::Row);
}

LayoutScope LayoutScope::panel(std::string_view label)
{
  return add(LayoutKind::Panel).set(LayoutAttrId::Label, label);
}

LayoutScope LayoutScope::tabs()
{
  return add(LayoutKind::Tabs);
}

LayoutScope LayoutScope::tab(std::string_view label)
{
  return add(LayoutKind::Tab).set(LayoutAttrId::Label, label);
}

LayoutScope LayoutScope::prop(std::string_view path)
{
  return add(LayoutKind::Prop).set(LayoutAttrId::Path, path);
}

LayoutScope LayoutScope::command(std::string_view invocation, std::string_view label)
{
  LayoutScope s = add(LayoutKind::Command);
  s.set(LayoutAttrId::Invocation, invocation);
  if (!label.empty()) {
    s.set(LayoutAttrId::Label, label);
  }
  return s;
}

LayoutScope LayoutScope::commandOptions(std::string_view target)
{
  return add(LayoutKind::CommandOptions).set(LayoutAttrId::Target, target);
}

LayoutScope LayoutScope::label(std::string_view text)
{
  return add(LayoutKind::Label).set(LayoutAttrId::Label, text);
}

LayoutScope LayoutScope::separator()
{
  return add(LayoutKind::Separator);
}

LayoutScope LayoutScope::custom(std::string_view tag)
{
  return add(LayoutKind::Custom).set(LayoutAttrId::Tag, tag);
}

} // namespace coredcc

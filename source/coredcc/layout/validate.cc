#include "coredcc/layout/validate.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/base/text.h"
#include "coredcc/command/command.h"
#include "coredcc/command/literal.h"

#include <string>
#include <utility>
#include <vector>

namespace coredcc {

using litestl::util::Vector;

namespace {

using Id = LayoutAttrId;
using Kind = ResolvedPath::Kind;

Status invalid(const std::string &what)
{
  return Status::error(StatusCode::InvalidArgument, what.c_str());
}

bool isProperty(const StaticTarget &t)
{
  return t.kind != Kind::Struct &&
         (t.kind != Kind::Member || t.member->kind() != MemberKind::Collection);
}

bool isCondition(const StaticTarget &t)
{
  return t.kind == Kind::EnumKey ||
         (t.kind == Kind::Member && t.member->def().kind() == PropKind::Bool);
}

class Validator {
 public:
  Validator(Runtime &rt, StructId root, const Layout &layout)
      : rt_(rt), layout_(layout), resolver_(rt), root_(resolver_.only(root))
  {
    if (root_.unknown) {
      issue(0, nullptr, LayoutSeverity::Error,
            Status::error(StatusCode::NotFound, "the root's struct isn't registered"));
    }
    prefixes_.resize(size_t(layout.nodeCount()));
  }

  Vector<LayoutIssue> run()
  {
    for (int i = 0; i < layout_.nodeCount() && !more_; i++) {
      node(i);
    }
    return std::move(issues_);
  }

 private:
  void issue(int node, const LayoutAttr *a, LayoutSeverity severity, const Status &s)
  {
    if (int(issues_.size()) == kMaxLayoutIssues) {
      more_ = true;
      issues_.last().severity = LayoutSeverity::Error;
      issues_.last().status = invalid("there are more issues");
      return;
    }
    std::string what = std::string("<") + layoutKindName(layout_.node(node).kind) + "> ";
    if (a && a->id != Id::Free) {
      what += std::string(layoutAttrName(a->id)) + ": ";
    }
    LayoutIssue out;
    out.node = node;
    out.offset = a ? a->offset : layout_.node(node).offset;
    out.severity = severity;
    out.status = Status::error(s.code(), (what + s.message()).c_str());
    for (const litestl::util::string &suggestion : s.suggestions()) {
      out.status.suggest(suggestion);
    }
    issues_.append(std::move(out));
  }
  void error(int node, const LayoutAttr *a, const Status &s)
  {
    issue(node, a, LayoutSeverity::Error, s);
  }
  void unknown(int node, const LayoutAttr *a)
  {
    issue(node, a, LayoutSeverity::Warning,
          Status::error(StatusCode::Unsupported,
                        "it can't be checked: it goes through a struct that isn't registered"));
  }

  /* Around a node's own paths. */
  const StaticStructs &around(int i) const
  {
    const int parent = layout_.node(i).parent;
    return parent < 0 ? root_ : prefixes_[size_t(parent)];
  }

  /* A layout's path: its own segments, and the structs they start from. */
  struct Start {
    Path path;
    const StaticStructs *from;
  };
  Start start(int i, std::string_view text) const
  {
    Start out;
    out.from = text[0] == '/' ? &root_ : &around(i);
    out.path = joinLayoutPath(Path(), text).valueOr(Path()); /* a Layout's paths parse */
    return out;
  }

  /* True if there may be something: what the prefix names has been reported. */
  bool canStart(const StaticStructs &from) const
  {
    return from.structs.size() > 0 || from.unknown;
  }

  /* Checks that one of what the path can name is as `accepts` says. */
  void target(int i,
              const LayoutAttr &a,
              std::string_view text,
              bool (*accepts)(const StaticTarget &),
              const char *what)
  {
    const Start s = start(i, text);
    if (!canStart(*s.from)) {
      return;
    }
    Result<StaticResolution> r = resolver_.resolve(*s.from, s.path);
    if (!r.ok()) {
      error(i, &a, r.status());
      return;
    }
    for (const StaticTarget &t : r->targets) {
      if (accepts(t)) {
        return;
      }
    }
    if (r->unknown) {
      if (!s.from->unknown) {
        unknown(i, &a);
      }
      return;
    }
    error(i, &a, invalid("'" + s.path.toString() + "' isn't " + what));
  }

  void node(int i)
  {
    const LayoutNode &n = layout_.node(i);
    prefixes_[size_t(i)] = around(i);
    for (uint32_t j = 0; j < n.attrCount && !more_; j++) {
      const LayoutAttr &a = layout_.attrs()[n.attrBegin + j];
      const std::string_view value = a.value.c_str();
      switch (a.id) {
        case Id::Path:
          if (n.kind == LayoutKind::Prop) {
            target(i, a, value, isProperty, "a property");
          }
          else {
            prefix(i, a, value, n.kind == LayoutKind::Custom);
          }
          break;
        case Id::VisibleIf:
        case Id::EnabledIf:
          target(i, a, value.substr(value[0] == '!' ? 1 : 0), isCondition,
                 "a Bool or an enum's key");
          break;
        case Id::Invocation:
          tool(i, a);
          break;
        case Id::Target:
          if (!rt_.ext<CommandRegistry>().find(value)) {
            error(i, &a, rt_.ext<CommandRegistry>().unknown(value));
          }
          break;
        default:
          break;
      }
    }
  }

  void prefix(int i, const LayoutAttr &a, std::string_view value, bool anything)
  {
    const Start s = start(i, value);
    StaticStructs &out = prefixes_[size_t(i)];
    if (!canStart(*s.from)) {
      out = StaticStructs();
      return;
    }
    Result<StaticStructs> r = resolver_.resolveStructs(*s.from, s.path);
    if (r.ok()) {
      if (r->unknown && !s.from->unknown && r->structs.size() == 0) {
        unknown(i, &a);
      }
      out = std::move(*r);
      return;
    }
    out = StaticStructs(); /* what is in it isn't reported again */
    Result<StaticResolution> any = resolver_.resolve(*s.from, s.path);
    if (!anything || !any.ok()) {
      error(i, &a, r.status());
    }
  }

  void tool(int i, const LayoutAttr &a)
  {
    Result<Invocation> inv = parseInvocation(a.value.c_str());
    if (!inv.ok()) {
      error(i, &a, inv.status()); /* a Layout's parse */
      return;
    }
    const CommandRegistry &commands = rt_.ext<CommandRegistry>();
    const CommandDef *def = commands.find(inv->command.c_str());
    if (!def) {
      error(i, &a, commands.unknown(inv->command.c_str()));
      return;
    }
    for (const InvocationArg &arg : inv->args) {
      if (more_) {
        break;
      }
      const PropertyDef *in = def->findInput(arg.name.c_str());
      if (!in) {
        error(i, &a, detail::unknownInput(*def, arg));
      }
      else if (Status s = argument(i, a, *in, arg); !s.ok()) {
        error(i, &a, s);
      }
    }
  }

  Status argument(int i, const LayoutAttr &a, const PropertyDef &in, const InvocationArg &arg)
  {
    const bool text = !arg.value.isTuple && arg.value.items.size() == 1 &&
                      arg.value.items[0].kind == RawScalar::Kind::String;
    if (in.kind() != PropKind::Pointer || !text) {
      return parseLiteral(rt_, in, arg.value).status();
    }
    const std::string_view s = arg.value.items[0].text.c_str();
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos || s[first] == '@') {
      Result<StableRef> ref = parseRef(rt_, s);
      return ref.ok() ? coercePointer(rt_, in, std::move(*ref)).status() : ref.status();
    }
    if (!canStart(around(i))) {
      return Status();
    }
    Result<Path> path = parsePath(s);
    if (!path.ok()) {
      return path.status();
    }
    Result<StaticStructs> to = resolver_.resolveStructs(around(i), *path);
    if (!to.ok()) {
      return to.status();
    }
    const StructId want = in.target()->target;
    bool fits = !want;
    for (StructId t : to->structs) {
      fits |= rt_.ext<ApiRegistry>().isA(t, want);
    }
    if (!fits && to->unknown) {
      if (!around(i).unknown) {
        unknown(i, &a);
      }
      return Status();
    }
    const ApiStruct *st = rt_.ext<ApiRegistry>().find(want);
    return fits ? Status() :
                  invalid(std::string(arg.name.c_str()) + ": '" + std::string(s) + "' isn't a " +
                          (st ? st->name().c_str() : "struct of its type"));
  }

  Runtime &rt_;
  const Layout &layout_;
  StaticResolver resolver_;
  StaticStructs root_;
  /** Of a container, for the paths in it. Nothing, and not unknown, if it was wrong. */
  std::vector<StaticStructs> prefixes_;
  Vector<LayoutIssue> issues_;
  /** There are more than that: it stops. */
  bool more_ = false;
};

} // namespace

Vector<LayoutIssue> validateLayout(Runtime &rt, StructId root, const Layout &layout)
{
  return Validator(rt, root, layout).run();
}

bool layoutIsValid(const Vector<LayoutIssue> &issues)
{
  for (const LayoutIssue &i : issues) {
    if (i.severity == LayoutSeverity::Error) {
      return false;
    }
  }
  return true;
}

} // namespace coredcc

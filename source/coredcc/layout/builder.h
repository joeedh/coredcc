#pragma once

/** \file
 * Building a Layout in C++.
 *
 *     LayoutBuilder b;
 *     LayoutScope panel = b.root().panel("Transform").path("objects.active");
 *     panel.prop("location");
 *     panel.row().command("object.clearLocation()", "Clear").visibleIf("!locked");
 *     Result<Layout> layout = b.finish();
 */

#include "coredcc/layout/layout.h"

#include <memory>
#include <string_view>

namespace coredcc {

struct DraftNode;
class LayoutBuilder;

/**
 * A node being built, and where its children go. Children may be added to any node at any
 * time; they keep the order they were added in. Valid while its builder is. Mistakes (a
 * child in a Prop, a path that doesn't parse) are finish()'s to report.
 */
class LayoutScope {
public:
  LayoutScope column();
  LayoutScope row();
  LayoutScope panel(std::string_view label);
  LayoutScope tabs();
  LayoutScope tab(std::string_view label);
  LayoutScope prop(std::string_view path);
  LayoutScope command(std::string_view invocation, std::string_view label = {});
  LayoutScope commandOptions(std::string_view target);
  LayoutScope label(std::string_view text);
  LayoutScope separator();
  LayoutScope custom(std::string_view tag);
  LayoutScope add(LayoutKind kind);

  /** Sets an attribute, over what it was. */
  LayoutScope &set(LayoutAttrId id, std::string_view value);
  /** `true` or `false`. (Not a `set`: a string literal converts to bool first.) */
  LayoutScope &setFlag(LayoutAttrId id, bool value);
  /** By name, as in XML: a Custom's own attributes too. */
  LayoutScope &set(std::string_view name, std::string_view value);
  LayoutScope &path(std::string_view p)
  {
    return set(LayoutAttrId::Path, p);
  }
  LayoutScope &id(std::string_view s)
  {
    return set(LayoutAttrId::Id, s);
  }
  LayoutScope &visibleIf(std::string_view condition)
  {
    return set(LayoutAttrId::VisibleIf, condition);
  }
  LayoutScope &enabledIf(std::string_view condition)
  {
    return set(LayoutAttrId::EnabledIf, condition);
  }
  /** In order of creation; messages of finish() name it. */
  int index() const
  {
    return index_;
  }

private:
  friend class LayoutBuilder;
  LayoutScope(LayoutBuilder &b, int index) : b_(&b), index_(index)
  {
  }

  LayoutBuilder *b_;
  int index_;
};

/** Builds one Layout. */
class LayoutBuilder {
public:
  LayoutBuilder();
  /** Starts with the nodes of `layout`, whose scopes are at() its indices: to add to it,
   * or to set attributes. */
  explicit LayoutBuilder(const Layout &layout);
  ~LayoutBuilder();
  LayoutBuilder(const LayoutBuilder &) = delete;
  LayoutBuilder &operator=(const LayoutBuilder &) = delete;

  LayoutScope root()
  {
    return LayoutScope(*this, 0);
  }
  /** A node by its index. */
  LayoutScope at(int node);
  /** The node with this `id` now, or -1. */
  int findId(std::string_view id) const;
  /** The layout, or what is wrong with it, as the parser would say. The first mistake
   * counts. The builder starts again, and its scopes are invalid. */
  Result<Layout> finish();

private:
  friend class LayoutScope;
  struct State;
  int add(int parent, LayoutKind kind);
  void set(int node, LayoutAttrId id, std::string_view name, std::string_view value);
  /* By name. */
  void set(int node, std::string_view name, std::string_view value);

  std::unique_ptr<State> s_;
};

} // namespace coredcc

#pragma once

/** \file
 * UI layouts: a tree of panels, properties and tools that frontends draw (architecture §12).
 * Layouts come from XML (layout/xml.h) or from C++ (layout/builder.h).
 */

#include "coredcc/base/status.h"
#include "coredcc/parse/path.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coredcc {

enum class LayoutKind : uint8_t {
  /** The root, and only the root. */
  Layout,
  Column,
  Row,
  Panel,
  /** Holds Tab nodes only. */
  Tabs,
  Tab,
  /** A property: `path`. */
  Prop,
  /** A button that runs a command: `invocation`. */
  Tool,
  /** The options of a command or tool: `target`. */
  ToolOptions,
  /** Text: `label`. */
  Label,
  Separator,
  /** The host's own: `tag`, free attributes, and text or children. */
  Custom,
};

enum class LayoutAttrId : uint8_t {
  /** Of a Prop, the property. Of a container, the prefix of what is in it. */
  Path,
  Label,
  Icon,
  Description,
  /** Unique in the layout. */
  Id,
  /** Style classes, separated by spaces. */
  Class,
  /** A condition: the path of a Bool, with `!` before it to negate it. */
  VisibleIf,
  EnabledIf,
  /** Tool: as in parse/invocation.h. */
  Invocation,
  /** ToolOptions: a command's id. */
  Target,
  /** Prop: a hint for the frontend, of any form. */
  Widget,
  /** Panel: a bool, closed at first. */
  Closed,
  /** Column, Row and Prop: a bool, fill the space there is. */
  Expand,
  /** Prop and Tool: a bool. */
  IconsOnly,
  /** Prop: a bool, true by default. */
  ShowLabel,
  /** Custom: what it is. */
  Tag,
  /** Custom: the text in the element. */
  Text,
  /** Custom: an attribute of any other name. */
  Free,
};

const char *layoutKindName(LayoutKind kind);
/** The attribute's name in XML and JSON. "" for Free. */
const char *layoutAttrName(LayoutAttrId id);

/** A node of a Layout. */
struct LayoutNode {
  LayoutKind kind = LayoutKind::Layout;
  /** Indices of nodes, or -1. */
  int32_t parent = -1;
  int32_t firstChild = -1;
  int32_t nextSibling = -1;
  /** Its attributes in Layout::attrs, in the order of their ids (Free ones by name). */
  uint32_t attrBegin = 0;
  uint32_t attrCount = 0;
  /** Where its element is in the text, for messages. 0 if it was built. */
  uint32_t offset = 0;
};

struct LayoutAttr {
  LayoutAttrId id = LayoutAttrId::Free;
  /** Free only. */
  litestl::util::string name;
  litestl::util::string value;
  /** Where it is in the text. 0 if it was built. */
  uint32_t offset = 0;
};

/** A `visibleIf` or an `enabledIf`. */
struct LayoutCondition {
  Path path;
  bool negated = false;
};

/** What a layout may hold, from text and from the builder. */
struct LayoutLimits {
  /** The root is at depth 1. */
  static constexpr int kMaxDepth = 64;
  static constexpr int kMaxNodes = 16384;
  static constexpr int kMaxAttrs = 64;
  /** Of the text. From the builder, of the attributes together. */
  static constexpr size_t kMaxBytes = 1u << 20;
  /** Of a path joined with the prefixes around it. */
  static constexpr size_t kMaxPathBytes = 1024;
};

/**
 * A layout. Immutable: parseLayout and LayoutBuilder make it, and both check the same. So the
 * paths and invocations in it are well formed, joined paths are within the limits, ids are
 * unique, and each node has the children and attributes that its kind allows. Whether the
 * paths name anything is validateLayout's to say.
 *
 * **Prefixes:** the `path` of a container is a prefix, joined with those of the containers it
 * is in, unless it starts with `/`, which means the Context's root. All of a node's own paths,
 * its `path` and its conditions, are relative to the prefix of its parent. A Tool's command runs
 * with that prefix as the Context's.
 */
class Layout {
 public:
  /** Just the root. */
  Layout();

  /** Nodes in preorder. Node 0 is the root. */
  int nodeCount() const
  {
    return int(nodes_.size());
  }
  const LayoutNode &node(int i) const
  {
    return nodes_[i];
  }
  const litestl::util::Vector<LayoutAttr> &attrs() const
  {
    return attrs_;
  }
  /** The node's attribute, or null. Not for Free. */
  const LayoutAttr *attr(int node, LayoutAttrId id) const;
  /** Its value, or "". */
  const char *text(int node, LayoutAttrId id) const;
  /** A bool attribute's value. */
  bool flag(int node, LayoutAttrId id, bool byDefault) const;
  /** The Free attribute, or null. */
  const LayoutAttr *freeAttr(int node, std::string_view name) const;
  /** The node with this `id`, or -1. */
  int findId(std::string_view id) const;

  /** The prefix that the node's own paths are relative to. Paths that a Layout gives are whole,
   * and start with `/`: they don't use a Context's prefix. */
  Path prefixOf(int node) const;
  /** The node's `path`, joined: a property's, or the prefix of a container's children. With
   * no `path`, its prefixOf. */
  Path effectivePath(int node) const;
  /** Of a ToolOptions: the struct of its command's last-used inputs, `/commandDefaults["id"]`.
   * The Context's root needs LastUsedStore::addCollectionTo. */
  Path optionsPath(int node) const;
  /** Its VisibleIf or EnabledIf, joined. False without one. */
  bool condition(int node, LayoutAttrId id, LayoutCondition &out) const;

  /** The same nodes and attributes. Offsets aren't compared. */
  bool operator==(const Layout &b) const;

 private:
  friend struct LayoutAssembler;
  litestl::util::Vector<LayoutNode> nodes_;
  litestl::util::Vector<LayoutAttr> attrs_;
};

/** `path` as layouts write it, joined with `prefix`: a leading `/` drops the prefix, and `/`
 * alone is the root. */
Result<Path> joinLayoutPath(const Path &prefix,
                            std::string_view path,
                            size_t *errorOffset = nullptr);

/**
 * The layout as JSON, for frontends that aren't C++. Each node is an object:
 * - `kind`: layoutKindName's;
 * - its attributes by name, the bools as booleans. A Custom's free ones are in `attrs`;
 * - `effectivePath` of a node with a `path`: the joined path. Joined paths start with `/`;
 * - `optionsPath` of a ToolOptions: what has its command's inputs as members;
 * - `visibleIf` and `enabledIf`: objects with the joined `path`, and `not` if it is negated;
 * - `prefix` of a Tool or ToolOptions, unless it is the root;
 * - `children`, if there are any.
 */
std::string layoutToJson(const Layout &layout);

} // namespace coredcc

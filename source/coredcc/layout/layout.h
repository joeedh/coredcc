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

/** The version of the format that this library reads and writes, as XML and as JSON. */
inline constexpr int kLayoutVersion = 1;

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
  /** Layout: the version of the format it is written for, from 1 (the default). */
  Version,
  /** Of a Prop, the property. Of a container, the prefix of what is in it. Of a Tool or a
   * ToolOptions, the prefix its command runs with, instead of that around it. */
  Path,
  Label,
  Icon,
  Description,
  /** Unique in the layout. */
  Id,
  /** Style classes, separated by spaces. */
  Class,
  /** A condition: a path, with `!` before it to negate it. See LayoutCondition. */
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
  /** `data-` and more: the host's own, on any node. Custom: an attribute of any other name. */
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
  /** Its effectivePath, in the Layout's paths. */
  uint32_t effectivePath = 0;
};

struct LayoutAttr {
  LayoutAttrId id = LayoutAttrId::Free;
  /** Free only. */
  litestl::util::string name;
  litestl::util::string value;
  /** Where it is in the text. 0 if it was built. */
  uint32_t offset = 0;
  /** Of a path or a condition: joined, in the Layout's paths. */
  uint32_t joined = 0;
};

/** Something in the text of a newer version that this one doesn't know: it isn't in the Layout. */
struct LayoutSkipped {
  /** The node that has the attribute, or that the element was in. */
  int node = 0;
  uint32_t offset = 0;
  /** "there is no element <x>", say. */
  litestl::util::string what;
};

/**
 * A `visibleIf` or an `enabledIf`. Its path names
 * - a Bool or an enum's key, which is true;
 * - a pointer, which isn't null;
 * - a collection's length, which isn't 0;
 * - a String, which isn't empty.
 *
 * A condition whose path has no value now is false, also if it is negated: what it is about
 * isn't there. So `!activeItem` is true without an active item, and `!activeItem.locked` isn't.
 */
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
  /** Of all joined paths, as text: those of `path`s and conditions, and tools' prefixes. It bounds
   * the JSON to a few times that and the text. */
  static constexpr size_t kMaxJoinedBytes = 2u << 20;
  /** More of LayoutSkipped aren't kept. */
  static constexpr int kMaxSkipped = 1000;
};

/**
 * A layout. Immutable: parseLayout and LayoutBuilder make it, and both check the same. So the
 * paths and invocations in it are well formed, joined paths are within the limits, ids are
 * unique, and each node has the children and attributes that its kind allows. Whether the
 * paths name anything is validateLayout's to say.
 *
 * **Versions:** in the text of a newer version (Version), elements and attributes of unknown names
 * are skipped, with what is in them, and are in `skipped`. Otherwise they are errors. A later
 * version that gives a name another meaning, or takes other values for an attribute, uses another
 * name. `list`, `menu`, `include` and `slot` are reserved for elements of later versions, also in
 * a Custom. `data-` attributes are the host's own, and no version defines one.
 *
 * **Prefixes:** the `path` of a container is a prefix, joined with those of the containers it
 * is in, unless it starts with `/`, which means the Context's root. All of a node's own paths,
 * its `path` and its conditions, are relative to the prefix of its parent. A Tool's command runs
 * with that prefix, or with the Tool's own `path`, as the Context's.
 *
 * **What isn't there:** a container whose own `path` names nothing now (a null pointer, an item that
 * is gone: reading it gives null or an error) is to be hidden, as if its VisibleIf were false, unless
 * validateLayout has an Error about that path, which is to be shown. A Custom is the host's to
 * decide about. A Prop without a value shows why, and a Tool is disabled if canInvoke says so.
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
  /** The version it is written for. */
  int version() const;
  /** What a newer version's text had that this one doesn't know. */
  const litestl::util::Vector<LayoutSkipped> &skipped() const
  {
    return skipped_;
  }

  /** The prefix that the node's own paths are relative to. Paths that a Layout gives are whole,
   * and start with `/`: they don't use a Context's prefix. They are made once, with the Layout. */
  const Path &prefixOf(int node) const;
  /** The node's `path`, joined: a property's, or the prefix of a container's children, or of a
   * Tool's command. With no `path`, its prefixOf. */
  const Path &effectivePath(int node) const;
  /** Of a ToolOptions: the struct of its command's last-used inputs, `/commandDefaults["id"]`.
   * The Context's root needs LastUsedStore::addCollectionTo. */
  Path optionsPath(int node) const;
  /** Its VisibleIf or EnabledIf, joined. False without one. */
  bool condition(int node, LayoutAttrId id, LayoutCondition &out) const;

  /** The same nodes and attributes. Offsets, and what was skipped, aren't compared. */
  bool operator==(const Layout &b) const;

 private:
  friend struct LayoutAssembler;
  litestl::util::Vector<LayoutNode> nodes_;
  litestl::util::Vector<LayoutAttr> attrs_;
  /** Joined ones. The first is the root. */
  litestl::util::Vector<Path> paths_;
  /** The nodes with an id, in the order of the ids. */
  litestl::util::Vector<int32_t> byId_;
  litestl::util::Vector<LayoutSkipped> skipped_;
};

/** `path` as layouts write it, joined with `prefix`: a leading `/` drops the prefix, and `/`
 * alone is the root. */
Result<Path> joinLayoutPath(const Path &prefix,
                            std::string_view path,
                            size_t *errorOffset = nullptr);

/**
 * The layout as JSON, for frontends that aren't C++. The root has `version`, the JSON's, which is
 * kLayoutVersion whatever the text's is. Each node is an object:
 * - `kind`: layoutKindName's;
 * - its attributes by name, the bools as booleans. The free ones are in `attrs`;
 * - `effectivePath` of a node with a `path`: the joined path. Joined paths start with `/`;
 * - `optionsPath` of a ToolOptions: what has its command's inputs as members;
 * - `visibleIf` and `enabledIf`: objects with the joined `path`, and `not` if it is negated;
 * - `prefix` of a Tool or ToolOptions, unless it is the root: what its command runs with;
 * - `children`, if there are any.
 */
std::string layoutToJson(const Layout &layout);

} // namespace coredcc

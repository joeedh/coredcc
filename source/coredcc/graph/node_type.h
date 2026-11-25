#pragma once

/** \file
 * Node types: a node's sockets and params, and the kernel that evaluates it.
 */

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/base/runtime.h"
#include "coredcc/graph/exec_context.h"
#include "coredcc/graph/socket_type.h"
#include "coredcc/props/def.h"

#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

/**
 * The code of a node type, native or foreign. One per type, with no per-node state: what
 * it computes depends only on its NodeExecContext. An error it returns fails the node.
 */
struct GraphKernel : IfaceRef {
  Status evaluate(NodeExecContext &ctx) const
  {
    return CDCC_CALL(evaluate, ctx);
  }

  CDCC_INTERFACE(GraphKernel, "coredcc::GraphKernel")
  {
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx"));
  }
};

/** Registers Impl under `implName` if it isn't yet, and makes one to be a node type's
 * kernel. Node types may share an Impl. */
template <class Impl>
Result<Owned<GraphKernel>> makeKernel(Runtime &rt, std::string_view implName)
{
  const litestl::binding::types::_StructBase *known =
      rt.ifaces().cachedImplDesc(TypeId::of(implName));
  if (!known || known->typeKey != litestl::binding::types::typeKeyOf<Impl>() ||
      !rt.ifaces().find(TypeId::of(implName), InterfaceId::of(GraphKernel::ifaceName)))
  {
    CDCC_TRY((registerImpl<GraphKernel, Impl>(rt, implName)));
  }
  return own<GraphKernel>(
      rt, implName, litestl::alloc::New<Impl>("coredcc::GraphKernel"));
}

/** A kernel that is one function. */
template <Status (*Fn)(NodeExecContext &)> struct FnKernel {
  Status evaluate(NodeExecContext &ctx) const
  {
    return Fn(ctx);
  }
  static litestl::binding::types::Struct<FnKernel> *defineBindings()
  {
    auto *st = new litestl::binding::types::Struct<FnKernel>("coredcc::FnKernel",
                                                             sizeof(FnKernel));
    BIND_STRUCT_METHOD(st, evaluate, MARGS("ctx"));
    return st;
  }
};

class GraphDef;

/** What a node of a type does. */
enum class NodeRole : uint8_t {
  /** Runs the type's kernel. */
  Kernel,
  /** `graph.input`: its outputs are the def's interface inputs. */
  Input,
  /** `graph.output`: its inputs are the def's interface outputs. */
  Output,
  /** `graph.group`: evaluates another def, whose interface gives its sockets. */
  Group,
  /** `graph.foreach`: evaluates another def for each element of a table
   * (graph/foreach.h). */
  Foreach,
};

/** Whether nodes of a role have a def that they evaluate. */
inline bool hasGroup(NodeRole r)
{
  return r == NodeRole::Group || r == NodeRole::Foreach;
}
/** Whether a node of a role needs all its inputs to give any output. */
inline bool isOneStep(NodeRole r)
{
  return r == NodeRole::Kernel || r == NodeRole::Foreach;
}

/** A socket as a node has it. */
struct SocketInfo {
  /** Its identifier, or `#<id>` for a socket of an interface. */
  litestl::util::string key;
  /** For display. */
  litestl::util::string name;
  litestl::util::string typeName;
};
using SocketInfos = litestl::util::Vector<SocketInfo, 1>;

/** An input of a node type. */
struct NodeInputDef {
  litestl::util::string name;
  litestl::util::string uiName;
  const SocketType *type = nullptr;
  /** The literal's def; null for a payload, which has no literal. Index into
   * NodeType::literalDefs, so payload inputs need no PropertyDef. */
  int literalDef = -1;
  /** An unlinked input's value while its literal isn't set. */
  GraphValue defaultValue;
};

/** An output of a node type. */
struct NodeOutputDef {
  litestl::util::string name;
  litestl::util::string uiName;
  const SocketType *type = nullptr;
};

struct NodeParamDef {
  PropertyDef def;
  /** Pointer params: the reference doesn't keep the block alive. */
  bool weak = false;
};

enum class NodeTypeFlags : uint32_t {
  None = 0,
  /** Its nodes write to something outside the graph, which a program that evaluates it
   * does from their outputs (GraphDef::forEachNode(flags) finds them). */
  OutputAdapter = 1 << 0,
  /**
   * The kernel may run on any thread, and at the same time as itself and others. Nothing
   * does so yet: kernels run one at a time on the main thread, and one without the flag
   * always will. A kernel that has it
   * - keeps no state of its own, or guards it;
   * - reads `userData` only, and only what nothing writes during an evaluation;
   * - doesn't use `library()`, `runtime()`, payloads that alias blocks, or scripts;
   * - calls useBlock, paramBlock, queryBlock and alias only before it does anything long,
   * as those may wait for the main thread.
   */
  ThreadSafe = 1 << 1,
};
constexpr NodeTypeFlags operator|(NodeTypeFlags a, NodeTypeFlags b)
{
  return NodeTypeFlags(uint32_t(a) | uint32_t(b));
}
constexpr NodeTypeFlags operator~(NodeTypeFlags a)
{
  return NodeTypeFlags(~uint32_t(a));
}
constexpr NodeTypeFlags operator&(NodeTypeFlags a, NodeTypeFlags b)
{
  return NodeTypeFlags(uint32_t(a) & uint32_t(b));
}

/** A registered node type. Lives as long as the Runtime, at a stable address. */
struct NodeType {
  litestl::util::string name;
  litestl::util::string uiName;
  NodeRole role = NodeRole::Kernel;
  NodeTypeFlags flags = NodeTypeFlags::None;
  uint32_t version = 1;
  /** Empty for roles other than Kernel, whose sockets come from interfaces. */
  litestl::util::Vector<NodeInputDef> inputs;
  litestl::util::Vector<NodeOutputDef> outputs;
  litestl::util::Vector<NodeParamDef> params;
  litestl::util::Vector<PropertyDef> literalDefs;
  /** `inputs` and `outputs` in the form GraphDef::sockets gives. */
  SocketInfos inputInfos;
  SocketInfos outputInfos;
  Owned<GraphKernel> kernel;
  /** Identifies this registration. */
  uint64_t stamp = 0;
  /** Moves when the kernel is replaced. */
  uint64_t kernelStamp = 0;

  /** These return -1 when there is none. */
  int inputIndex(std::string_view inputName) const;
  int outputIndex(std::string_view outputName) const;
  int paramIndex(std::string_view paramName) const;
  /** The def of input `i`'s literal, or null for a payload. */
  const PropertyDef *literal(int i) const
  {
    const int d = inputs[i].literalDef;
    return d >= 0 ? &literalDefs[d] : nullptr;
  }
};

/**
 * Describes a node type, for NodeTypeRegistry::add.
 *
 *     NodeTypeDef("my.scale").uiName("Scale")
 *         .input(PropertyDef::floatProp("factor").range(0, 10))
 *         .payloadInput("mesh", "my.Mesh")
 *         .output("out", "my.Mesh")
 *         .param(PropertyDef::enumProp("mode").item("FAST", 0).item("EXACT", 1))
 *         .kernel(std::move(kernel));
 *
 * An input's PropertyDef gives its socket type and describes its literal. Enum and Flags
 * values aren't socket types: make them params. Without a `uiName`, sockets are shown
 * by one that is derived from their name, as properties are.
 */
class NodeTypeDef {
public:
  /** `name` is dotted identifiers, e.g. "my.scale". */
  explicit NodeTypeDef(std::string_view name);
  NodeTypeDef &uiName(std::string_view s);
  NodeTypeDef &flags(NodeTypeFlags f);
  /** 1, unless set. Raise it when nodes of the type in files need a migration: a socket
   * or param that has another name or meaning now. Nodes keep the version they were made
   * with (GraphNode::typeVersion), in files too. */
  NodeTypeDef &version(uint32_t v);
  NodeTypeDef &input(PropertyDef def);
  NodeTypeDef &payloadInput(std::string_view name,
                            std::string_view socketType,
                            std::string_view uiName = {});
  NodeTypeDef &output(std::string_view name,
                      std::string_view socketType,
                      std::string_view uiName = {});
  /** A Pointer param refers to a block. With `weak`, it doesn't keep the block alive. */
  NodeTypeDef &param(PropertyDef def, bool weak = false);
  NodeTypeDef &kernel(Owned<GraphKernel> kernel);

  const litestl::util::string &name() const
  {
    return name_;
  }
  /** What was given to `input` or `param` under that name, to change it; null if none
   * was. */
  PropertyDef *inputDef(std::string_view name);
  PropertyDef *paramDef(std::string_view name);
  /** Sets the uiName of a socket; false if there is none of that name. */
  bool setSocketUiName(bool output, std::string_view name, std::string_view uiName);
  /** A copy without the kernel. */
  NodeTypeDef copy() const;
  /** Bound without members: scripts use bind/'s facades. */
  static const litestl::binding::types::Struct<NodeTypeDef> *defineBindings();

private:
  friend class NodeTypeRegistry;
  struct Input {
    litestl::util::string name;
    litestl::util::string uiName;
    /** A payload input's socket type; else `def` gives it. */
    litestl::util::string payloadType;
    /** Index in literalDefs_, or -1. */
    int literalDef = -1;
  };
  struct Output {
    litestl::util::string name;
    litestl::util::string uiName;
    litestl::util::string socketType;
  };
  litestl::util::string name_;
  litestl::util::string uiName_;
  NodeTypeFlags flags_ = NodeTypeFlags::None;
  uint32_t version_ = 1;
  litestl::util::Vector<Input> inputs_;
  litestl::util::Vector<Output> outputs_;
  litestl::util::Vector<PropertyDef> literalDefs_;
  litestl::util::Vector<NodeParamDef> params_;
  Owned<GraphKernel> kernel_;
};

inline constexpr const char *kGraphInputType = "graph.input";
inline constexpr const char *kGraphOutputType = "graph.output";
inline constexpr const char *kGraphGroupType = "graph.group";
inline constexpr const char *kGraphForeachType = "graph.foreach";

/**
 * Node types by name. A Runtime extension, created with the built-in ones: `graph.input`,
 * `graph.output`, `graph.group` and `graph.foreach`, `value.<socket type>` (a constant,
 * in its `value` param), `math.float`, `vec.combine`, `vec.separate` and `mat4.multiply`,
 * and those for tables (graph/foreach.h):
 * - `elements.range`: `count` elements, with the columns `index` and `factor` (0 to 1);
 * - `elements.count`;
 * - `elements.value.<socket type>`: the value of a column at an index;
 * - `elements.rename`: renames a column;
 * - `elements.combine`: a's rows, with b's columns.
 *
 * Types can't be removed, and only their kernels replaced. Registering is Busy while a
 * graph evaluates, and counts as an edit, so nodes waiting for the type evaluate again.
 */
class NodeTypeRegistry {
public:
  static constexpr const char *kExtName = "coredcc.NodeTypeRegistry";

  explicit NodeTypeRegistry(Runtime *rt);
  ~NodeTypeRegistry();
  NodeTypeRegistry(const NodeTypeRegistry &) = delete;
  NodeTypeRegistry &operator=(const NodeTypeRegistry &) = delete;

  /**
   * Registers a type. Everything is checked before anything changes: names are unique
   * identifiers other than `length` and `active`, defs are valid, socket types exist, a
   * Pointer param names its target struct, there are no Collection params, no dynamic
   * defs and no Optional inputs, the version isn't 0, and there is a kernel and an
   * output.
   */
  Result<const NodeType *> add(NodeTypeDef def);
  /**
   * Replaces the kernel of a registered type by that of `def`, for code that is reloaded.
   * All nodes of the type run again when they are needed. FailedPrecondition unless the
   * rest says the same as the type: the names, types and order of sockets and params, the
   * items of enums, and the version. Texts, ranges and defaults aren't compared, and stay
   * the type's.
   */
  Status replaceKernel(NodeTypeDef def);
  /** What replaceKernel would say of `def`, but for its kernel. */
  Status checkReplace(const NodeTypeDef &def) const;
  /** What `add` would say of `def`, but for its kernel. */
  Status checkDef(const NodeTypeDef &def) const;
  /** The type, or null. */
  const NodeType *find(std::string_view name) const;
  /** The NotFound error for `name`, with suggestions. */
  Status unknown(std::string_view name) const;
  int size() const
  {
    return int(types_.size());
  }
  const NodeType &at(int i) const
  {
    return *types_[i];
  }
  /** Moves with every registration. */
  uint64_t stamp() const
  {
    return stamp_;
  }
  Status validate() const;

private:
  /** A type made of `def`, without its kernel. Owned by the caller (alloc::Delete). */
  Result<NodeType *> build(const NodeTypeDef &def, bool replaces = false) const;
  Result<const NodeType *> store(NodeType *type);

  friend class GraphDef;

  Runtime &rt_;
  uint64_t stamp_ = 0;
  /** The Runtime's defs, which are told of new types. */
  GraphDef *defs_ = nullptr;
  litestl::util::Vector<NodeType *> types_;
  litestl::util::Map<uint64_t, NodeType *> byId_;
};

} // namespace coredcc

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
  return own<GraphKernel>(rt, implName, litestl::alloc::New<Impl>("coredcc::GraphKernel"));
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
};

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
  const SocketType *type = nullptr;
};

struct NodeParamDef {
  PropertyDef def;
  /** Pointer params: the reference doesn't keep the block alive. */
  bool weak = false;
};

/** A registered node type. Lives as long as the Runtime, at a stable address. */
struct NodeType {
  litestl::util::string name;
  litestl::util::string uiName;
  NodeRole role = NodeRole::Kernel;
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
 * values aren't socket types: make them params.
 */
class NodeTypeDef {
 public:
  /** `name` is dotted identifiers, e.g. "my.scale". */
  explicit NodeTypeDef(std::string_view name);
  NodeTypeDef &uiName(std::string_view s);
  NodeTypeDef &input(PropertyDef def);
  NodeTypeDef &payloadInput(std::string_view name, std::string_view socketType);
  NodeTypeDef &output(std::string_view name, std::string_view socketType);
  /** A Pointer param refers to a block. With `weak`, it doesn't keep the block alive. */
  NodeTypeDef &param(PropertyDef def, bool weak = false);
  NodeTypeDef &kernel(Owned<GraphKernel> kernel);

 private:
  friend class NodeTypeRegistry;
  struct Input {
    litestl::util::string name;
    /** A payload input's socket type; else `def` gives it. */
    litestl::util::string payloadType;
    /** Index in literalDefs_, or -1. */
    int literalDef = -1;
  };
  struct Output {
    litestl::util::string name;
    litestl::util::string socketType;
  };
  litestl::util::string name_;
  litestl::util::string uiName_;
  litestl::util::Vector<Input> inputs_;
  litestl::util::Vector<Output> outputs_;
  litestl::util::Vector<PropertyDef> literalDefs_;
  litestl::util::Vector<NodeParamDef> params_;
  Owned<GraphKernel> kernel_;
};

inline constexpr const char *kGraphInputType = "graph.input";
inline constexpr const char *kGraphOutputType = "graph.output";
inline constexpr const char *kGraphGroupType = "graph.group";

/**
 * Node types by name. A Runtime extension, created with the built-in ones: `graph.input`,
 * `graph.output` and `graph.group`, `value.<socket type>` (a constant, in its `value`
 * param), `math.float`, `vec.combine`, `vec.separate` and `mat4.multiply`.
 *
 * Types can't be replaced or removed. Registering is Busy while a graph evaluates, and
 * counts as an edit, so nodes waiting for the type evaluate again.
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
   * defs and no Optional inputs, and there is a kernel and an output.
   */
  Result<const NodeType *> add(NodeTypeDef def);
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
  Result<const NodeType *> store(NodeType *type);

  Runtime &rt_;
  uint64_t stamp_ = 0;
  litestl::util::Vector<NodeType *> types_;
  litestl::util::Map<uint64_t, NodeType *> byId_;
};

namespace detail {
/** Registers the built-in kernel types (builtin_nodes.cc). */
void addBuiltinNodes(Runtime &rt, NodeTypeRegistry &reg);
} // namespace detail

} // namespace coredcc

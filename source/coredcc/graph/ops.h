#pragma once

/** \file
 * Undoable graph edits: GraphEdit makes them and GraphEditRecord keeps what their undo
 * and redo need. Commands compose these (graph/commands.h).
 */

#include "coredcc/api/context.h"
#include "coredcc/command/history_block.h"
#include "coredcc/graph/def.h"

#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <memory>
#include <span>
#include <string_view>

namespace coredcc {

/** About the bytes a value takes, for undo budgets. */
uint64_t memSizeOf(const PropValue &v);
uint64_t memSizeOf(const NodeValues &values);

/** One edit, as data. Which fields are used goes by the kind. */
struct GraphOp {
  enum class Kind : uint8_t {
    /** `node` with `links`; `inverse`: its removal. */
    Node,
    /** `links[0]`; `inverse`: its removal. */
    Link,
    Label,
    Position,
    Param,
    Literal,
    /** An extra (`key`). */
    Extra,
    /** A Pointer param (`key`), or the group (an empty key). */
    Ref,
    /** `socket` on `side` at `indexAfter`; `inverse`: its removal. */
    Interface,
    MoveInterface,
    InterfaceType,
    InterfaceName,
    InterfaceDefault,
  };
  Kind kind = Kind::Node;
  bool inverse = false;
  /** Of the node, or the interface socket. */
  int64_t id = 0;
  litestl::util::string key;
  PropValue before, after;
  /** The stored values of a param or literal; empty when it isn't set. */
  NodeValues valuesBefore, valuesAfter;
  NodeExtras extraBefore, extraAfter;
  BlockId blockBefore, blockAfter;
  std::unique_ptr<NodeState> node;
  litestl::util::Vector<LinkState, 1> links;
  std::unique_ptr<InterfaceSocket> socket;
  SocketSide side = SocketSide::Input;
  int indexBefore = 0, indexAfter = 0;

  /** Does it again (`forward`), or takes it back. */
  Status apply(GraphDef &def, bool forward) const;
  uint64_t byteSize() const;
};

/**
 * The edits of one undo step, on one graph block. It finds the graph by its block's id
 * each time, so the block may have been replaced under it, and pins (PinKind::History)
 * the block and every block its edits removed, added or replaced a reference to, until
 * `discard`.
 */
class GraphEditRecord {
public:
  GraphEditRecord() = default;

  /** Takes the edits back; a failure becomes the Runtime's sticky error.
   * FailedPrecondition when the Library is gone, or `ctx` is of another. */
  void undo(Context &ctx);
  /** Makes them again; a failure takes back what it did first. The same precondition. */
  Status redo(Context &ctx);
  void discard();
  uint64_t byteSize() const;
  int size() const
  {
    return int(ops_.size());
  }

private:
  friend class GraphEdit;

  Result<GraphDef *> def(Context &ctx) const;
  void pin(Library &lib, BlockId id);
  Status revert(GraphDef &def, int from);

  HistoryBlock block_;
  litestl::util::Vector<GraphOp> ops_;
  litestl::util::Vector<LibraryPin> pins_;
  /** The blocks of `pins_`. */
  litestl::util::Map<BlockHandle, bool> pinned_;
};

/**
 * Edits a graph block and records how to undo it. The calls are GraphDef's, with its
 * checks and errors; one that fails changes and records nothing.
 */
class GraphEdit {
public:
  /** FailedPrecondition if `id` isn't a Live graph block of the Context's Library. */
  static Result<GraphEdit> begin(Context &ctx, BlockId id, GraphEditRecord &record);

  const GraphDef &def() const
  {
    return *def_;
  }
  /** Takes back everything, and forgets it. */
  Status revert();

  Result<NodeId> addNode(std::string_view type);
  Status insertNode(const NodeState &state);
  Status removeNode(NodeId id);
  Status setLabel(NodeId id, std::string_view label);
  Status setPosition(NodeId id, double x, double y);
  Status setExtra(NodeId id, std::string_view name, const PropValue &value);
  Status setParam(NodeId id, std::string_view name, const PropValue &value);
  Status setLiteral(NodeId id, std::string_view input, const PropValue &value);
  Status useLiteral(NodeId id, std::string_view input, bool on);
  Status setGroup(NodeId id, BlockId def);
  /** Also InvalidArgument for interior sockets (graph/interior.h). */
  Result<LinkId> link(SocketRef from, SocketRef to);
  /** GraphDef::linkAll. The same. */
  Status linkAll(std::span<const LinkSpec> links, litestl::util::Vector<LinkId> &ids);
  Status insertLink(const LinkState &state);
  Status unlink(LinkId id);
  Result<int64_t> addInterface(SocketSide side,
                               std::string_view name,
                               std::string_view socketType,
                               int index = -1);
  Status removeInterface(int64_t id);
  Status moveInterface(int64_t id, int index);
  Status setInterfaceType(int64_t id, std::string_view socketType);
  Status setInterfaceName(int64_t id, std::string_view name);
  Status setInterfaceDefault(int64_t id, const PropValue &value);

private:
  GraphEdit(GraphDef *def, Library *lib, GraphEditRecord *record)
      : def_(def), lib_(lib), record_(record)
  {
  }
  void push(GraphOp op);
  template <class Fn> Status literalEdit(NodeId id, std::string_view input, Fn &&fn);

  GraphDef *def_;
  Library *lib_;
  GraphEditRecord *record_;
};

using GraphEditFn = litestl::util::function<Status(GraphEdit &)>;

/**
 * Runs many edits of one graph as one undo step, named `name`, through the Context's
 * executor (as a sub-command inside another command's exec). If `fn` fails, what it did
 * is taken back and its error returned.
 */
Status runGraphEdit(Context &ctx, BlockId graph, std::string_view name, GraphEditFn fn);

} // namespace coredcc

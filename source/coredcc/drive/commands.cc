/* core.driver.add and core.driver.remove: the document's list of drivers. */
#include "coredcc/drive/commands.h"
#include "coredcc/api/change_bus.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/history_block.h"
#include "coredcc/drive/driver_list.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/registry.h"
#include "coredcc/scene/commands.h"

#include "litestl/binding/binding.h"

namespace coredcc::drive {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

namespace {

Status failed(const char *why)
{
  return Status::error(StatusCode::FailedPrecondition, why);
}

/* The Library, unless a list of drivers is a placeholder: editing another list then
 * would leave it to drive again once it is read. */
Result<Library *> libraryIn(Context &ctx)
{
  if (!ctx.library()) {
    return failed("the Context has no Library");
  }
  for (BlockHandle h : DriverList::allLists(*ctx.library())) {
    if (ctx.library()->get(h)->state() == BlockState::Placeholder) {
      return failed("the document's list of drivers isn't readable yet");
    }
  }
  return ctx.library();
}

/* The input `graph`: a Live graph block. */
Result<BlockHandle> graphInput(Context &ctx)
{
  Result<BlockHandle> h = detail::blockOf(ctx, "graph");
  if (!h.ok()) {
    return h.status();
  }
  if (!*h || !graphDefOf(*ctx.library(), *h)) {
    return Status::error(StatusCode::InvalidArgument, "graph: that isn't a graph");
  }
  return *h;
}

void report(Context &ctx, const Status &s)
{
  if (!s.ok() && !ctx.runtime().hasStickyError()) {
    ctx.runtime().setStickyError(s); /* undo can't return a Status */
  }
}

/* The places of a list's cells that name the graph, ascending. */
Vector<int> placesOf(const Library &lib, const DriverList &list, const BlockId &graph)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  Vector<int> places;
  for (int i = 0; i < int(list.cells().size()); i++) {
    if (clients.canonical(list.cells()[i].target) == clients.canonical(graph)) {
      places.append(i);
    }
  }
  return places;
}

/* The Live lists that name the graph, in the Library's order. */
Vector<BlockHandle> listsNaming(const Library &lib, const BlockId &graph)
{
  Vector<BlockHandle> out;
  const TypeId type = TypeId::of(kDriverListType);
  lib.forEach([&](BlockHandle h, const Datablock &b) {
    const DriverList *l = b.type() == type && b.state() == BlockState::Live
                              ? DriverList::of(lib, h)
                              : nullptr;
    if (l && placesOf(lib, *l, graph).size() > 0) {
      out.append(h);
    }
  });
  return out;
}

/* Cells of one list that name the graph, at `places` (ascending). */
struct ListCells {
  HistoryBlock list;
  Vector<int> places;
};

/* The cells an add or a removal changed, in each list: put back, or removed again. */
struct GraphCells {
  LibraryRef lib;
  HistoryBlock graph;
  Vector<ListCells> lists;

  static Result<DriverList *> listOf(const ListCells &l, Library &in)
  {
    DriverList *d = DriverList::of(in, l.list.get());
    if (!d) {
      return failed("the list of drivers is gone");
    }
    return d;
  }
  Status add(Context &ctx) const
  {
    Result<Library *> in = detail::libraryOf(ctx, lib);
    CDCC_TRY(in.status());
    for (const ListCells &l : lists) {
      Result<DriverList *> d = listOf(l, **in);
      CDCC_TRY(d.status());
      for (int place : l.places) {
        CDCC_TRY((*d)->insertCell(**in, graph.id(), place));
      }
      changed(ctx, l);
    }
    return Status();
  }
  Status remove(Context &ctx) const
  {
    Result<Library *> in = detail::libraryOf(ctx, lib);
    CDCC_TRY(in.status());
    for (const ListCells &l : lists) {
      Result<DriverList *> d = listOf(l, **in);
      CDCC_TRY(d.status());
      CDCC_TRY((*d)->remove(**in, graph.id()).status());
      changed(ctx, l);
    }
    return Status();
  }
  static void changed(Context &ctx, const ListCells &l)
  {
    ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(l.list.id()), "graphs");
  }
  uint64_t byteSize() const
  {
    uint64_t n = sizeof(*this) + uint64_t(lists.size()) * sizeof(ListCells);
    for (const ListCells &l : lists) {
      n += uint64_t(l.places.size()) * sizeof(int);
    }
    return n;
  }
  void release()
  {
    graph.release();
    for (ListCells &l : lists) {
      l.list.release();
    }
  }
};

struct AddGraph {
  GraphCells cells;
  /* The list, when this made it. */
  CreatedBlocks created;

  Status canRun(Context &ctx) const
  {
    Result<Library *> lib = libraryIn(ctx);
    CDCC_TRY(lib.status());
    Result<BlockHandle> g = graphInput(ctx);
    CDCC_TRY(g.status());
    if (listsNaming(**lib, (*lib)->get(*g)->id()).size() > 0) {
      return Status::error(StatusCode::AlreadyExists,
                           "graph: a list of drivers names it");
    }
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(canRun(ctx));
    Library &lib = *ctx.library();
    const BlockHandle g = *graphInput(ctx);
    BlockHandle h = DriverList::find(lib);
    if (h.isNull()) {
      Result<BlockHandle> made = newDriverList(lib);
      CDCC_TRY(made.status());
      h = *made;
      created.add(lib, h);
    }
    DriverList &list = *DriverList::of(lib, h);
    CDCC_TRY(list.insert(lib, lib.get(g)->id()));
    ListCells l;
    l.list = HistoryBlock(lib, h);
    l.places.append(int(list.cells().size()) - 1);
    cells.lib = lib.ref();
    cells.graph = HistoryBlock(lib, g);
    cells.lists.append(std::move(l));
    GraphCells::changed(ctx, cells.lists[0]);
    return Status();
  }
  void undo(Context &ctx)
  {
    report(ctx, cells.remove(ctx));
    if (created.size() > 0) {
      Result<Library *> lib = detail::libraryOf(ctx, cells.lib);
      if (lib.ok()) {
        created.undo(**lib);
      }
      report(ctx, lib.status());
    }
  }
  Status redo(Context &ctx)
  {
    if (created.size() > 0) {
      Result<Library *> lib = detail::libraryOf(ctx, cells.lib);
      CDCC_TRY(lib.status());
      CDCC_TRY(created.redo(**lib));
    }
    return cells.add(ctx);
  }
  uint64_t byteSize() const
  {
    return cells.byteSize() + created.byteSize();
  }
  void discard()
  {
    if (Library *lib = cells.lib.get()) {
      created.discard(*lib);
    }
    cells.release();
  }
  static lbt::Struct<AddGraph> *defineBindings()
  {
    auto *st =
        new lbt::Struct<AddGraph>("coredcc::AddDriverGraphCommand", sizeof(AddGraph));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, byteSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

/* Removes every cell that names the graph, in every list; undo brings back each. */
struct RemoveGraph {
  GraphCells cells;

  Status canRun(Context &ctx) const
  {
    Result<Library *> lib = libraryIn(ctx);
    CDCC_TRY(lib.status());
    Result<BlockHandle> g = detail::blockOf(ctx, "graph");
    if (!g.ok() || !*g) {
      return g.ok() ? Status::error(StatusCode::InvalidArgument, "graph: none is given")
                    : g.status();
    }
    if (listsNaming(**lib, (*lib)->get(*g)->id()).size() == 0) {
      return Status::error(StatusCode::NotFound, "graph: no list of drivers names it");
    }
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(canRun(ctx));
    Library &lib = *ctx.library();
    const BlockHandle g = *detail::blockOf(ctx, "graph");
    const BlockId id = lib.get(g)->id();
    cells.lib = lib.ref();
    cells.graph = HistoryBlock(lib, g);
    for (BlockHandle h : listsNaming(lib, id)) {
      DriverList &list = *DriverList::of(lib, h);
      ListCells l;
      l.list = HistoryBlock(lib, h);
      l.places = placesOf(lib, list, id);
      CDCC_TRY(list.remove(lib, id).status());
      GraphCells::changed(ctx, l);
      cells.lists.append(std::move(l));
    }
    return Status();
  }
  void undo(Context &ctx)
  {
    report(ctx, cells.add(ctx));
  }
  Status redo(Context &ctx)
  {
    return cells.remove(ctx);
  }
  uint64_t byteSize() const
  {
    return cells.byteSize();
  }
  void discard()
  {
    cells.release();
  }
  static lbt::Struct<RemoveGraph> *defineBindings()
  {
    auto *st = new lbt::Struct<RemoveGraph>("coredcc::RemoveDriverGraphCommand",
                                            sizeof(RemoveGraph));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, byteSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

} // namespace

Status registerCommands(Runtime &rt)
{
  CommandDef add(kAddDriverCommand);
  add.uiName("Add Driver Graph")
      .description("The graph drives the document's objects.")
      .input(PropertyDef::pointerProp("graph", StructId()));
  CDCC_TRY(detail::addCommandOnce<AddGraph>(rt, std::move(add)));
  CommandDef remove(kRemoveDriverCommand);
  remove.uiName("Remove Driver Graph")
      .input(PropertyDef::pointerProp("graph", StructId()));
  return detail::addCommandOnce<RemoveGraph>(rt, std::move(remove));
}

} // namespace coredcc::drive

/* core.scene.addGraph and core.scene.removeGraph: the graphs a scene lists. */
#include "coredcc/drive/commands.h"
#include "coredcc/api/change_bus.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/history_block.h"
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

/* The input `scene`, or the Context's scene. */
Result<detail::SceneOf> sceneFrom(Context &ctx)
{
  Result<BlockHandle> h = detail::blockOf(ctx, "scene");
  if (!h.ok()) {
    return h.status();
  }
  if (!*h) {
    return detail::sceneOf(ctx);
  }
  detail::SceneOf s;
  s.lib = ctx.library();
  s.handle = *h;
  s.scene = Scene::of(*s.lib, *h);
  if (!s.scene) {
    return Status::error(StatusCode::InvalidArgument, "scene: that isn't a scene");
  }
  return s;
}

/* The scene and the graph, as an undo record keeps them. */
struct Held {
  HistoryBlock scene;
  HistoryBlock graph;

  Result<Scene *> get(const Context &ctx, Library *&lib) const
  {
    Result<Library *> l = detail::libraryOf(ctx, scene.library());
    if (!l.ok()) {
      return l.status();
    }
    lib = *l;
    Scene *s = Scene::of(*lib, scene.get());
    if (!s) {
      return failed("the scene is gone");
    }
    return s;
  }
  void changed(Context &ctx) const
  {
    ctx.runtime().ext<ChangeBus>().notify(OwnerKey::ofBlock(scene.id()), "graphs");
  }
  void release()
  {
    scene.release();
    graph.release();
  }
};

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

/* The places of the cells that name the graph, in order. */
Vector<int> placesOf(const Library &lib, const Scene &scene, const BlockId &graph)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  Vector<int> places;
  for (int i = 0; i < int(scene.graphCells().size()); i++) {
    if (clients.canonical(scene.graphCells()[i].target) == clients.canonical(graph)) {
      places.append(i);
    }
  }
  return places;
}

/* Adds cells at `places` (ascending), or removes all that name the graph. */
struct GraphCells {
  Held held;
  Vector<int> places;

  Status add(Context &ctx) const
  {
    Library *lib = nullptr;
    Result<Scene *> s = held.get(ctx, lib);
    CDCC_TRY(s.status());
    for (int place : places) {
      CDCC_TRY((*s)->insertGraphCell(*lib, held.graph.id(), place));
    }
    held.changed(ctx);
    return Status();
  }
  Status remove(Context &ctx) const
  {
    Library *lib = nullptr;
    Result<Scene *> s = held.get(ctx, lib);
    CDCC_TRY(s.status());
    CDCC_TRY((*s)->removeGraph(*lib, held.graph.id()).status());
    held.changed(ctx);
    return Status();
  }
  uint64_t memSize() const
  {
    return sizeof(*this) + uint64_t(places.size()) * sizeof(int);
  }
  void discard()
  {
    held.release();
  }
};

struct AddGraph {
  GraphCells cells;

  Status canRun(Context &ctx) const
  {
    Result<detail::SceneOf> s = sceneFrom(ctx);
    if (!s.ok()) {
      return s.status();
    }
    return graphInput(ctx).status();
  }
  Status exec(Context &ctx)
  {
    Result<detail::SceneOf> s = sceneFrom(ctx);
    CDCC_TRY(s.status());
    Result<BlockHandle> g = graphInput(ctx);
    CDCC_TRY(g.status());
    CDCC_TRY(s->scene->insertGraph(*s->lib, s->lib->get(*g)->id()));
    cells.places.clear();
    cells.places.append(int(s->scene->graphCells().size()) - 1);
    cells.held.scene = HistoryBlock(*s->lib, s->handle);
    cells.held.graph = HistoryBlock(*s->lib, *g);
    cells.held.changed(ctx);
    return Status();
  }
  void undo(Context &ctx)
  {
    report(ctx, cells.remove(ctx));
  }
  Status redo(Context &ctx)
  {
    return cells.add(ctx);
  }
  uint64_t memSize() const
  {
    return cells.memSize();
  }
  void discard()
  {
    cells.discard();
  }
  static lbt::Struct<AddGraph> *defineBindings()
  {
    auto *st =
        new lbt::Struct<AddGraph>("coredcc::AddSceneGraphCommand", sizeof(AddGraph));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

/* Removes every cell that names the graph (a file may have two); undo brings back each.
 */
struct RemoveGraph {
  GraphCells cells;

  Status canRun(Context &ctx) const
  {
    Result<detail::SceneOf> s = sceneFrom(ctx);
    if (!s.ok()) {
      return s.status();
    }
    Result<BlockHandle> g = detail::blockOf(ctx, "graph");
    if (!g.ok() || !*g) {
      return g.ok() ? Status::error(StatusCode::InvalidArgument, "graph: none is given")
                    : g.status();
    }
    if (placesOf(*s->lib, *s->scene, s->lib->get(*g)->id()).size() == 0) {
      return Status::error(StatusCode::NotFound, "graph: the scene doesn't list it");
    }
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(canRun(ctx));
    Result<detail::SceneOf> s = sceneFrom(ctx);
    const BlockHandle g = *detail::blockOf(ctx, "graph");
    const BlockId id = s->lib->get(g)->id();
    cells.places = placesOf(*s->lib, *s->scene, id);
    CDCC_TRY(s->scene->removeGraph(*s->lib, id).status());
    cells.held.scene = HistoryBlock(*s->lib, s->handle);
    cells.held.graph = HistoryBlock(*s->lib, g);
    cells.held.changed(ctx);
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
  uint64_t memSize() const
  {
    return cells.memSize();
  }
  void discard()
  {
    cells.discard();
  }
  static lbt::Struct<RemoveGraph> *defineBindings()
  {
    auto *st = new lbt::Struct<RemoveGraph>("coredcc::RemoveSceneGraphCommand",
                                            sizeof(RemoveGraph));
    BIND_STRUCT_METHOD(st, canRun, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, exec, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, memSize, MARGS());
    BIND_STRUCT_METHOD(st, discard, MARGS());
    return st;
  }
};

PropertyDef sceneInput()
{
  return PropertyDef::pointerProp("scene", StructId()); /* the Context's when unset */
}

} // namespace

Status registerCommands(Runtime &rt)
{
  CommandDef add(kAddGraphCommand);
  add.uiName("Add Graph to Scene")
      .description("The scene lists the graph, which then drives the document's objects.")
      .input(PropertyDef::pointerProp("graph", StructId()))
      .input(sceneInput());
  CDCC_TRY(detail::addCommandOnce<AddGraph>(rt, std::move(add)));
  CommandDef remove(kRemoveGraphCommand);
  remove.uiName("Remove Graph from Scene")
      .input(PropertyDef::pointerProp("graph", StructId()))
      .input(sceneInput());
  return detail::addCommandOnce<RemoveGraph>(rt, std::move(remove));
}

} // namespace coredcc::drive

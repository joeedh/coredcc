/* core.scene.addGraph and core.scene.removeGraph: the graphs a scene lists. */
#include "coredcc/drive/commands.h"
#include "coredcc/api/change_bus.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/history_block.h"
#include "coredcc/graph/provider.h"
#include "coredcc/scene/commands.h"

#include "litestl/binding/binding.h"

namespace coredcc::drive {

namespace lbt = litestl::binding::types;

namespace {

Status failed(const char *why)
{
  return Status::error(StatusCode::FailedPrecondition, why);
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

struct AddGraph {
  Held held;
  int index = -1;

  Status canRun(Context &ctx) const
  {
    Result<detail::SceneOf> s = detail::sceneOf(ctx);
    if (!s.ok()) {
      return s.status();
    }
    return graphInput(ctx).status();
  }
  Status exec(Context &ctx)
  {
    Result<detail::SceneOf> s = detail::sceneOf(ctx);
    CDCC_TRY(s.status());
    Result<BlockHandle> g = graphInput(ctx);
    CDCC_TRY(g.status());
    CDCC_TRY(s->scene->insertGraph(*s->lib, s->lib->get(*g)->id()));
    index = int(s->scene->graphCells().size()) - 1;
    held.scene = HistoryBlock(*s->lib, s->handle);
    held.graph = HistoryBlock(*s->lib, *g);
    held.changed(ctx);
    return Status();
  }
  Status add(Context &ctx) const
  {
    Library *lib = nullptr;
    Result<Scene *> s = held.get(ctx, lib);
    CDCC_TRY(s.status());
    CDCC_TRY((*s)->insertGraph(*lib, held.graph.id(), index));
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
  void undo(Context &ctx)
  {
    report(ctx, remove(ctx));
  }
  Status redo(Context &ctx)
  {
    return add(ctx);
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard()
  {
    held.release();
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

struct RemoveGraph {
  AddGraph inverse;

  Status canRun(Context &ctx) const
  {
    Result<detail::SceneOf> s = detail::sceneOf(ctx);
    if (!s.ok()) {
      return s.status();
    }
    Result<BlockHandle> g = detail::blockOf(ctx, "graph");
    if (!g.ok() || !*g) {
      return g.ok() ? Status::error(StatusCode::InvalidArgument, "graph: none is given")
                    : g.status();
    }
    const BlockId id = s->lib->get(*g)->id();
    for (const RefCell &c : s->scene->graphCells()) {
      if (s->lib->resolveRef(c) == *g || c.target == id) {
        return Status();
      }
    }
    return Status::error(StatusCode::NotFound, "graph: the scene doesn't list it");
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(canRun(ctx));
    Result<detail::SceneOf> s = detail::sceneOf(ctx);
    const BlockHandle g = *detail::blockOf(ctx, "graph");
    Result<int> at = s->scene->removeGraph(*s->lib, s->lib->get(g)->id());
    CDCC_TRY(at.status());
    inverse.index = *at;
    inverse.held.scene = HistoryBlock(*s->lib, s->handle);
    inverse.held.graph = HistoryBlock(*s->lib, g);
    inverse.held.changed(ctx);
    return Status();
  }
  void undo(Context &ctx)
  {
    report(ctx, inverse.add(ctx));
  }
  Status redo(Context &ctx)
  {
    return inverse.remove(ctx);
  }
  uint64_t memSize() const
  {
    return sizeof(*this);
  }
  void discard()
  {
    inverse.discard();
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

} // namespace

Status registerCommands(Runtime &rt)
{
  CommandDef add(kAddGraphCommand);
  add.uiName("Add Graph to Scene")
      .description("The scene lists the graph, which then drives the document's objects.")
      .input(PropertyDef::pointerProp("graph", StructId()));
  CDCC_TRY(detail::addCommandOnce<AddGraph>(rt, std::move(add)));
  CommandDef remove(kRemoveGraphCommand);
  remove.uiName("Remove Graph from Scene")
      .input(PropertyDef::pointerProp("graph", StructId()));
  return detail::addCommandOnce<RemoveGraph>(rt, std::move(remove));
}

} // namespace coredcc::drive

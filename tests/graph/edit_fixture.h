#pragma once

/* A document for the graph datapath and command tests: a root struct with the collections
 * `graphs` and `things` (Live blocks by name), an executor and a history. */

#include "graph_fixture.h"

#include "coredcc/api/resolve.h"
#include "coredcc/api/struct.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/set_property.h"
#include "coredcc/graph/api.h"
#include "coredcc/graph/commands.h"

namespace fixture {

inline PointerRef blockPointer(const Library &lib, BlockHandle h, StructId type)
{
  const Datablock *b = lib.get(h);
  PointerRef r;
  if (b) {
    r.data = lib.data(h);
    r.type = type;
    r.owner = OwnerKey::ofBlock(b->id());
  }
  return r;
}

inline CollectionAccessor blocksByName(TypeId type, StructId item)
{
  CollectionAccessor c;
  auto all = [type](Context &ctx) {
    litestl::util::Vector<BlockHandle> out;
    ctx.library()->forEach([&](BlockHandle h, const Datablock &b) {
      if (b.type() == type && b.state() == BlockState::Live) {
        out.append(h);
      }
    });
    return out;
  };
  c.length = [all](const PointerRef &, Context &ctx) { return int64_t(all(ctx).size()); };
  c.at = [all, item](const PointerRef &, Context &ctx, int64_t i) {
    return blockPointer(*ctx.library(), all(ctx)[int(i)], item);
  };
  c.lookup = [all, item](const PointerRef &, Context &ctx, const KeySegment &key) {
    for (BlockHandle h : all(ctx)) {
      if (strcmp(ctx.library()->get(h)->name().c_str(), key.text.c_str()) == 0) {
        return blockPointer(*ctx.library(), h, item);
      }
    }
    return PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &it) {
    return KeySegment::stringKey(
        ctx.library()->get(ctx.library()->find(it.owner.block))->name().c_str());
  };
  c.itemStruct = item;
  return c;
}

/* A float that a `Thing` struct shows. */
inline void addDocument(Runtime &rt)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  rt.ext<NodeTypeRegistry>();
  ApiStruct thing("Thing");
  thing.addField(PropertyDef::floatProp("value"), &Thing::value);
  test_assert(reg.add(std::move(thing)).ok());
  const StructId graphs = StructId::of(kGraphDefType);
  ApiStruct doc("Doc");
  doc.addCollection(PropertyDef::collectionProp("graphs", graphs),
                    blocksByName(TypeId::of(kGraphDefType), graphs));
  doc.addCollection(PropertyDef::collectionProp("things", kThingStruct),
                    blocksByName(kThing, kThingStruct));
  test_assert(reg.add(std::move(doc)).ok());
  static int root = 0;
  test_assert(
      reg.addRuntimeOwner("doc", StructId::of("Doc"), []() { return static_cast<void *>(&root); })
          .ok());
}

/* Types, blocks, commands and the document. */
struct DocRuntime : Runtime {
  DocRuntime()
  {
    addThings(*this);
    test_assert(registerGraphCommands(*this).ok() && registerBlockCommands(*this).ok());
    addDocument(*this);
  }
};

/* Declared in the order they must be destroyed in, backwards. */
struct Doc {
  DocRuntime rt;
  Library lib;
  LinearHistory history;
  CommandExecutor executor;
  Context ctx;

  Doc()
      : lib(&rt), executor(&rt, &history),
        ctx(&rt, &lib, rt.ext<ApiRegistry>().runtimeOwner("doc"))
  {
    ctx.setExecutor(&executor);
    lib.setQuarantine(true);
    lib.setAuditRelations(true);
  }
  ~Doc()
  {
    test_assert(rt.validateAll().ok() && lib.validate().ok() && history.validate().ok());
    test_assert(!rt.hasStickyError());
    history.clear();
  }

  Result<PropValue> get(const std::string &path)
  {
    return getValue(ctx, path);
  }
  /* Through SetProperty, so it can be undone. */
  Status set(const std::string &path, const PropValue &v)
  {
    return setValueUndo(ctx, path, v);
  }
  bool is(const std::string &path, const PropValue &v)
  {
    Result<PropValue> got = get(path);
    if (!got.ok()) {
      printf("%s: %s\n", path.c_str(), got.status().toString().c_str());
    }
    return got.ok() && *got == v;
  }
  Status undo()
  {
    return executor.undo(ctx);
  }
  Status redo()
  {
    return executor.redo(ctx);
  }

  struct Arg {
    const char *name;
    PropValue value;
  };
  Result<PropertyBag> run(const char *command, std::initializer_list<Arg> args)
  {
    const CommandDef *def = rt.ext<CommandRegistry>().find(command);
    test_assert(def);
    PropertyBag in = def->newInputs();
    for (const Arg &a : args) {
      test_assert(in.set(a.name, a.value).ok());
    }
    return executor.run(ctx, *def, std::move(in));
  }
  /* The Int output `name` of a command; 0 when it fails. */
  int64_t runFor(const char *output, const char *command, std::initializer_list<Arg> args)
  {
    Result<PropertyBag> out = run(command, args);
    if (!out.ok()) {
      printf("%s: %s\n", command, out.status().toString().c_str());
      return 0;
    }
    return out->get(output)->asInt();
  }
};

inline PropValue pointerTo(BlockId id, StructId type = StructId())
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  r.type = type;
  return PropValue::ofPointer(r);
}
inline PropValue pointerTo(const Graph &g)
{
  return pointerTo(g.id, StructId::of(kGraphDefType));
}

} // namespace fixture

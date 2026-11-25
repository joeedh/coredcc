/* Collections with an activeMember: writing `.active`, its undo, and its watchers. */
#include "command_fixture.h"

#include "coredcc/parse/path_cache.h"

test_init;

/* A stage: the World's objects and an active one in a Weak cell, whose setter checks
 * membership. Registered as the runtime owner "stage". */
struct Stage {
  World *w;
  RefCell active{RefKind::Weak};
  int updates = 0;
  bool fixed = false; /* activeObject is read-only */

  explicit Stage(World &world, const char *activeMember = "activeObject") : w(&world)
  {
    ApiRegistry &reg = world.rt.ext<ApiRegistry>();
    ApiStruct s("Stage");
    CollectionAccessor c;
    c.length = [](const PointerRef &self, Context &) {
      return int64_t(static_cast<Stage *>(self.data)->w->scene.objects.size());
    };
    c.at = [](const PointerRef &self, Context &ctx, int64_t i) {
      return blockRef(*ctx.library(), static_cast<Stage *>(self.data)->w->scene.objects[int(i)],
                      kObject);
    };
    c.lookup = [](const PointerRef &, Context &, const KeySegment &) { return PointerRef(); };
    c.keyOf = [](const PointerRef &, Context &, const PointerRef &) {
      return KeySegment::ofHandle(1);
    };
    const auto get = [](const PointerRef &self, Context &ctx) {
      Library &lib = *ctx.library();
      return blockRef(lib, lib.resolveRef(static_cast<Stage *>(self.data)->active), kObject);
    };
    c.active = get;
    c.activeMember = activeMember;
    PointerAccessor p;
    p.get = get;
    p.set = [](const PointerRef &self, Context &ctx, const PointerRef &target) {
      Stage *st = static_cast<Stage *>(self.data);
      BlockId id;
      if (!target.isNull()) {
        id = target.owner.block;
        if (!st->w->scene.objects.contains(ctx.library()->find(id))) {
          return Status::error(StatusCode::FailedPrecondition, "not on the stage");
        }
      }
      return ctx.library()->writeRef(st->active, id);
    };
    p.cell = [](const PointerRef &self, Context &) {
      return &static_cast<Stage *>(self.data)->active;
    };
    s.addCollection(PropertyDef::collectionProp("objects", kObject), std::move(c));
    s.addPointer(PropertyDef::pointerProp("activeObject", kObject), std::move(p))
        .update([](const PointerRef &self, Context &) {
          static_cast<Stage *>(self.data)->updates++;
        })
        .dynamic([](PropertyDef &def, const PointerRef &self, Context &) {
          if (static_cast<Stage *>(self.data)->fixed) {
            def.access(PropAccess::ReadOnly);
          }
        });
    s.addField(PropertyDef::boolProp("fixed"), &Stage::fixed);
    test_assert(reg.add(std::move(s)).ok());
    test_assert(
        reg.addRuntimeOwner("stage", StructId::of("Stage"), [this]() { return (void *)this; })
            .ok());
  }
};

struct Fixture : Setup {
  Stage stage{w};
  Context sctx{&w.rt, &w.lib, w.rt.ext<ApiRegistry>().runtimeOwner("stage")};

  Fixture()
  {
    sctx.setExecutor(&ex);
  }
  PropValue object(int i)
  {
    StableRef r;
    r.owner = OwnerKey::ofBlock(w.objectId(i));
    r.type = kObject;
    return PropValue::ofPointer(r);
  }
  BlockHandle active()
  {
    return w.lib.resolveRef(stage.active);
  }
};

static Path P(const char *s)
{
  Result<Path> p = parsePath(s);
  test_assert(p.ok());
  return std::move(p).value();
}

static void test_writes()
{
  Fixture f;
  Context &ctx = f.sctx;
  /* From no item. Reads are as before; the member reads as a null pointer. */
  test_assert(getValue(ctx, "objects.active.mode").status().code() == StatusCode::NotFound);
  test_assert(resolve(ctx, "objects.active").status().code() == StatusCode::NotFound);
  Result<ResolvedPath> rp = resolveForWrite(ctx, "objects.active");
  test_assert(rp.ok() && rp->kind == ResolvedPath::Kind::Member);
  test_assert(strcmp(rp->member->name(), "activeObject") == 0);
  test_assert(rp->canonical == *canonicalize(ctx, P("activeObject")));

  test_assert(setValue(ctx, "objects.active", f.object(1)).ok());
  test_assert(f.active() == f.w.scene.objects[1] && f.stage.updates == 1);
  test_assert(resolve(ctx, "objects.active")->kind == ResolvedPath::Kind::Struct);
  test_assert(*getValue(ctx, "objects.active.name") == PropValue::ofString("Sphere"));
  test_assert(*getValue(ctx, "activeObject") == f.object(1));
  /* A path through it isn't the member. */
  test_assert(setValue(ctx, "objects.active.mode", PropValue::ofEnum(1)).ok());
  test_assert(f.w.object(1).mode == 1 && f.stage.updates == 1);
  test_assert(resolveForWrite(ctx, "objects[0]")->kind == ResolvedPath::Kind::Struct);
  test_assert(setValue(ctx, "objects[0]", f.object(0)).code() == StatusCode::FailedPrecondition);

  /* Refusals change nothing. */
  StableRef mesh;
  mesh.owner = OwnerKey::ofBlock(f.w.meshId(0));
  mesh.type = kMesh;
  test_assert(setValue(ctx, "objects.active", PropValue::ofPointer(mesh)).code() ==
              StatusCode::TypeMismatch);
  test_assert(setValue(ctx, "objects.active", PropValue::ofInt(0)).code() ==
              StatusCode::TypeMismatch);
  Result<BlockHandle> other =
      f.w.lib.create(kObjectType, "Other", litestl::alloc::New<fixture::Object>("Object"));
  StableRef o;
  o.owner = OwnerKey::ofBlock(f.w.lib.get(*other)->id());
  test_assert(setValue(ctx, "objects.active", PropValue::ofPointer(o)).code() ==
              StatusCode::FailedPrecondition);
  f.stage.fixed = true;
  test_assert(setValue(ctx, "objects.active", f.object(0)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(setValueUndo(ctx, "objects.active", f.object(0)).code() ==
              StatusCode::FailedPrecondition);
  f.stage.fixed = false;
  {
    EvaluationScope scope(f.w.rt);
    test_assert(setValue(ctx, "objects.active", f.object(0)).code() == StatusCode::Busy);
  }
  test_assert(f.active() == f.w.scene.objects[1] && f.history.size() == 0);
  test_assert(setValue(ctx, "objects.active", PropValue()).ok() && !f.active());

  /* A collection without an activeMember: as before. */
  test_assert(setValue(f.ctx, "objects.active", f.object(1)).code() ==
              StatusCode::FailedPrecondition);
  test_assert(!setValueUndo(f.ctx, "objects.active", f.object(1)).ok());
}

static void test_undo()
{
  Fixture f;
  Context &ctx = f.sctx;
  /* A null old item, a fold (also with the member's own path), a clear. */
  test_assert(setValueUndo(ctx, "objects.active", f.object(0), 7).ok());
  test_assert(setValueUndo(ctx, "objects.active", f.object(1), 7).ok());
  test_assert(setValueUndo(ctx, "activeObject", f.object(1), 7).ok());
  test_assert(f.history.size() == 1);
  test_assert(setValueUndo(ctx, "objects.active", PropValue()).ok() && !f.active());
  test_assert(f.ex.undo(ctx).ok() && f.active() == f.w.scene.objects[1]);
  test_assert(f.ex.undo(ctx).ok() && !f.active());
  test_assert(f.ex.redo(ctx).ok() && f.active() == f.w.scene.objects[1]);

  /* The collection's order doesn't matter to undo. */
  test_assert(setValueUndo(ctx, "objects.active", f.object(0)).ok());
  std::swap(f.w.scene.objects[0], f.w.scene.objects[1]);
  test_assert(f.ex.undo(ctx).ok() && f.active() == f.w.scene.objects[0]);
  test_assert(f.history.validate().ok() && f.w.lib.validate().ok());
}

static void test_undo_of_blocks_not_live()
{
  Fixture f;
  Context &ctx = f.sctx;
  const PropValue first = f.object(0), second = f.object(1);
  test_assert(setValueUndo(ctx, "objects.active", first).ok());
  /* Delete the active object, set another, undo both. */
  test_assert(f.w.lib.markDeleted(f.w.scene.objects[0]).ok() && !f.active());
  test_assert(setValueUndo(ctx, "objects.active", second).ok());
  test_assert(f.ex.undo(ctx).ok() && !f.active());
  test_assert(f.w.lib.restoreFromHistory(f.w.scene.objects[0]).ok());
  test_assert(f.active() == f.w.scene.objects[0]);
  /* Redo, then the same with a Detached block. */
  test_assert(f.ex.redo(ctx).ok() && f.active() == f.w.scene.objects[1]);
  test_assert(f.w.lib.onHostDestroyed(f.w.objectId(1)).ok() && !f.active());
  test_assert(setValueUndo(ctx, "objects.active", first).ok());
  test_assert(f.ex.undo(ctx).ok() && !f.active());
  test_assert(f.stage.active.target == f.w.objectId(1));
}

static void test_inner_collection()
{
  Setup s;
  Result<ResolvedPath> b = resolve(s.ctx, "tool.brushes[1]");
  test_assert(b.ok());
  test_assert(setValueUndo(s.ctx, "tool.brushes.active", PropValue::ofPointer(b->canonical)).ok());
  test_assert(s.w.tool.activeBrush == 1);
  test_assert(canonicalize(s.ctx, P("tool.brushes.active"))->toString() == b->canonical.toString());
  test_assert(s.ex.undo(s.ctx).ok() && s.w.tool.activeBrush == 0);
  test_assert(s.ex.redo(s.ctx).ok() && s.w.tool.activeBrush == 1);
}

static void test_watchers()
{
  /* Inside one runtime owner neither the epoch nor the Library moves: only the touched
   * member says that the path means another item. */
  Setup s;
  s.w.tool.brushes[1].size = 9;
  Context tool(&s.w.rt, nullptr, s.w.rt.ext<ApiRegistry>().runtimeOwner("tool"));
  ChangeBus &bus = s.w.rt.ext<ChangeBus>();
  double size = 0;
  int calls = 0;
  Result<ScopedConnection> c =
      bus.watch(s.ctx, P("tool.brushes.active.size"), [&](const PropValue &v, const Status &) {
        size = v.tryFloat().value_or(-1);
        calls++;
      });
  test_assert(c.ok());
  bus.flush();
  calls = 0;
  Result<ResolvedPath> b = resolve(tool, "brushes[1]");
  test_assert(b.ok());
  test_assert(setValue(tool, "brushes.active", PropValue::ofPointer(b->canonical)).ok());
  bus.flush();
  test_assert(calls == 1 && size == 9);

  /* A watcher of the member, for a write through `.active` and its undo. */
  Fixture f;
  ChangeBus &fb = f.w.rt.ext<ChangeBus>();
  int fired = 0;
  Result<ScopedConnection> m =
      fb.watch(f.sctx, P("activeObject"), [&](const PropValue &, const Status &) { fired++; });
  test_assert(m.ok());
  fb.flush();
  fired = 0;
  test_assert(setValueUndo(f.sctx, "objects.active", f.object(1)).ok());
  fb.flush();
  test_assert(fired == 1 && f.ex.undo(f.sctx).ok());
  fb.flush();
  test_assert(fired == 2);
}

static void test_registration()
{
  /* Not a member, not a pointer. A write through it is an error. */
  for (const char *member : {"nope", "fixed", "objects"}) {
    World w;
    Stage st(w, member);
    const Status s = w.rt.ext<ApiRegistry>().validate();
    test_assert(s.code() == StatusCode::InvalidArgument);
    test_assert(hasMsg(s, "isn't a settable pointer"));
    Context ctx(&w.rt, &w.lib, w.rt.ext<ApiRegistry>().runtimeOwner("stage"));
    test_assert(resolveForWrite(ctx, "objects.active").status().code() == StatusCode::Internal);
  }
  {
    World w;
    Stage st(w);
    test_assert(w.rt.validateAll().ok());
  }
  /* A pointer to the items' base struct is fine. Not: another struct; one without a setter; a collection without `active`. */
  const char *expect[] = {"points to another struct", "isn't a settable pointer",
                          "without `active`"};
  for (int i = 0; i < 4; i++) {
    World w;
    ApiRegistry &reg = w.rt.ext<ApiRegistry>();
    ApiStruct s("List");
    CollectionAccessor c;
    c.length = [](const PointerRef &, Context &) { return int64_t(0); };
    c.at = [](const PointerRef &, Context &, int64_t) { return PointerRef(); };
    c.lookup = [](const PointerRef &, Context &, const KeySegment &) { return PointerRef(); };
    c.keyOf = [](const PointerRef &, Context &, const PointerRef &) {
      return KeySegment::ofHandle(0);
    };
    if (i != 2) {
      c.active = [](const PointerRef &, Context &) { return PointerRef(); };
    }
    c.activeMember = "current";
    PointerAccessor p;
    p.get = [](const PointerRef &, Context &) { return PointerRef(); };
    if (i != 1) {
      p.set = [](const PointerRef &, Context &, const PointerRef &) { return Status(); };
    }
    s.addCollection(PropertyDef::collectionProp("list", i == 3 ? kMesh : kObject), std::move(c));
    s.addPointer(PropertyDef::pointerProp("current", i == 0 ? kMesh : i == 3 ? kIdBase : kObject), std::move(p));
    test_assert(reg.add(std::move(s)).ok());
    test_assert(i == 3 ? reg.validate().ok() : hasMsg(reg.validate(), expect[i]));
  }
}

static void test_scene_slot()
{
  Setup s;
  ChangeBus &bus = s.w.rt.ext<ChangeBus>();
  test_assert(!s.ctx.scene());
  const uint64_t e = bus.epoch();
  s.ctx.setScene(s.w.scene.objects[0]);
  test_assert(s.ctx.scene() == s.w.scene.objects[0] && bus.epoch() > e);
  const uint64_t e2 = bus.epoch();
  s.ctx.setScene(s.w.scene.objects[0]); /* the same */
  test_assert(bus.epoch() == e2);
  Context other = s.w.context(); /* one per Context */
  test_assert(!other.scene());
  s.ctx.setScene(BlockHandle());
  test_assert(!s.ctx.scene() && bus.epoch() > e2);
}

int main()
{
  test_writes();
  test_undo();
  test_undo_of_blocks_not_live();
  test_inner_collection();
  test_watchers();
  test_registration();
  test_scene_slot();
  return test_end();
}

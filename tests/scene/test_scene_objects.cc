/* The input `objects` of the commands that take several objects: it must do what the same
 * objects do when they are selected. */
#include "command_doc.h"

#include "coredcc/api/ref_text.h"

test_init;

using namespace scene_test;

namespace {

PropValue text(const std::string &s)
{
  return PropValue::ofString(s.c_str());
}

std::string ids(CommandDoc &d, std::initializer_list<BlockHandle> handles)
{
  std::string s;
  for (BlockHandle h : handles) {
    s += (s.empty() ? "" : ";") + formatBlockId(d.rt, d.idOf(h));
  }
  return s;
}

std::string names(CommandDoc &d, const Vector<BlockHandle> &handles)
{
  std::string s;
  for (BlockHandle h : handles) {
    s += d.lib->get(h)->name().c_str();
  }
  return s;
}

std::string selected(CommandDoc &d)
{
  return names(d, d.sc().selected(*d.lib));
}

struct Case {
  const char *command;
  std::vector<std::pair<const char *, PropValue>> inputs;
};

/* Objects a, b (under a), c (under b), e, f, with a selection that is none of those
 * named. */
struct Five : CommandDoc {
  BlockHandle a, b, c, e, f;
  Five()
  {
    a = object("a", 1);
    b = object("b", 2);
    c = object("c", 3);
    e = object("e", 4);
    f = object("f", 5);
    setParent(b, a);
    setParent(c, b);
    select({f});
  }
  Result<PropertyBag> runCase(const Case &k, const std::string *objects)
  {
    const CommandDef *def = rt.ext<CommandRegistry>().find(k.command);
    PropertyBag bag = def->newInputs();
    for (const auto &[name, value] : k.inputs) {
      must(bag.set(name, value));
    }
    if (objects) {
      must(bag.set("objects", text(*objects)));
    }
    return ex->run(*ctx, *def, std::move(bag));
  }
};

std::vector<Case> cases()
{
  return {
      {kUnlinkSceneCommand, {}},
      {kSetObjectParentCommand, {}},
      {kSetObjectParentCommand, {{"keepWorld", PropValue::ofBool(false)}}},
      {kSetObjectTransformTypeCommand, {{"type", text(kMatrixTransform)}}},
      {kSetObjectRotationModeCommand,
       {{"mode", PropValue::ofEnum(trs::kQuaternionMode)}}},
      {kApplyObjectDeltaCommand, {{"vector", PropValue::ofVec3(1, 2, 3)}}},
      {kDeleteObjectCommand, {}},
      {kDeleteObjectCommand, {{"keepChildrenWorld", PropValue::ofBool(false)}}},
  };
}

void testSameAsSelected()
{
  for (const Case &k : cases()) {
    for (int which = 0; which < 4; which++) {
      Five s, n;
      const auto pick = [which](Five &d) {
        return which == 0   ? std::vector<BlockHandle>{d.c, d.a}
               : which == 1 ? std::vector<BlockHandle>{d.b}
               : which == 2 ? std::vector<BlockHandle>{d.e, d.b, d.c}
                            : std::vector<BlockHandle>{d.a, d.b, d.c, d.e, d.f};
      };
      Vector<BlockId> chosen;
      for (BlockHandle h : pick(s)) {
        chosen.append(s.idOf(h));
      }
      s.sc().setSelection(*s.lib, chosen);
      std::string named;
      for (BlockHandle h : pick(n)) {
        named += formatBlockId(n.rt, n.idOf(h)) + ";";
      }
      named += named; /* each twice, with a ';' at the end */
      const std::string before = n.describe();
      Result<PropertyBag> x = s.runCase(k, nullptr), y = n.runCase(k, &named);
      test_assert(x.ok() && y.ok());
      if (!x.ok() || !y.ok()) {
        fprintf(
            stderr, "%s: %s %s\n", k.command, x.status().message(), y.status().message());
        continue;
      }
      for (const char *out : {"count", "approximate"}) {
        const PropValue *p = x->get(out), *q = y->get(out);
        test_assert(!p == !q && (!p || p->asInt() == q->asInt()));
      }
      /* The documents differ in their ids only. */
      const std::string after = n.describe();
      for (int i = 0; i < 5; i++) {
        const BlockHandle hs = (&s.a)[i], hn = (&n.a)[i];
        const bool live = s.lib->get(hs)->state() == BlockState::Live;
        test_assert(live == (n.lib->get(hn)->state() == BlockState::Live));
        if (!live) {
          continue;
        }
        test_assert(s.obj(hs).channels == n.obj(hn).channels);
        test_assert(s.obj(hs).transformTypeId() == n.obj(hn).transformTypeId());
        const BlockHandle ps = s.lib->resolveRef(s.obj(hs).parent.cell());
        const BlockHandle pn = n.lib->resolveRef(n.obj(hn).parent.cell());
        test_assert(!ps == !pn &&
                    (!ps || s.lib->get(ps)->name() == n.lib->get(pn)->name()));
      }
      test_assert(names(s, s.sc().objects(*s.lib)) == names(n, n.sc().objects(*n.lib)));
      test_assert(after != before);
      for (int i = 0; i < 2; i++) {
        n.undo();
        test_assert(CommandDoc::liveOnly(n.describe()) == CommandDoc::liveOnly(before));
        n.redo();
        test_assert(CommandDoc::liveOnly(n.describe()) == CommandDoc::liveOnly(after));
      }
    }
  }
}

void testRefusals()
{
  std::vector<Case> all = cases();
  all.push_back({kSelectSceneCommand, {{"mode", PropValue::ofEnum(0)}}});
  all.push_back({kSelectSceneCommand, {{"mode", PropValue::ofEnum(3)}}});
  for (const Case &k : all) {
    Five d;
    const BlockHandle outside = d.loose("outside"), shape = d.shapeBlock("shape", 1.0),
                      gone = d.object("gone");
    const std::string goneId = ids(d, {gone});
    must(d.lib->markDeleted(gone));
    const std::string good = ids(d, {d.a, d.e});
    struct Bad {
      std::string objects;
      StatusCode code;
      const char *message;
    };
    const Bad bad[] = {
        {"nonsense", StatusCode::InvalidArgument, "objects: "},
        {good + ",", StatusCode::InvalidArgument, "objects: "},
        {good + ";coredcc,12", StatusCode::InvalidArgument, "objects: "},
        {good + ";;" + good, StatusCode::InvalidArgument, "objects: "},
        {good + ";coredcc,0123456789abcdef0123456789abcdef",
         StatusCode::NotFound,
         "coredcc,0123456789abcdef0123456789abcdef"},
        {good + ";" + goneId, StatusCode::NotFound, goneId.c_str()},
        {good + ";" + ids(d, {outside}),
         StatusCode::FailedPrecondition,
         "'outside': isn't in"},
        {ids(d, {shape}) + ";" + good,
         StatusCode::FailedPrecondition,
         "'shape': isn't in"},
    };
    for (const Bad &b : bad) {
      const std::string before = d.describe();
      const int steps = d.history->size();
      const Status s = d.runCase(k, &b.objects).status();
      if (!isError(s, b.code, b.message)) {
        fprintf(stderr, "%s(%s): %s\n", k.command, b.objects.c_str(), s.message());
      }
      test_assert(isError(s, b.code, b.message));
      test_assert(d.describe() == before && d.history->size() == steps);
    }
    if (strcmp(k.command, kSelectSceneCommand) != 0) {
      const std::string none;
      test_assert(isError(
          d.runCase(k, &none).status(), StatusCode::FailedPrecondition, "objects"));
    }
  }
}

void testSelect()
{
  Five d;
  enum { kSet, kAdd, kRemove, kToggle, kAll, kNone };
  const auto select = [&](int m, const std::string &objects) {
    d.cycle(kSelectSceneCommand,
            {{"mode", PropValue::ofEnum(m)}, {"objects", text(objects)}});
    return selected(d);
  };
  test_assert(select(kSet, ids(d, {d.e, d.a, d.e})) == "ea");
  test_assert(select(kAdd, ids(d, {d.f, d.a, d.c})) == "eafc");
  test_assert(select(kRemove, ids(d, {d.f, d.b, d.e})) == "ac");
  test_assert(select(kToggle, ids(d, {d.c, d.b, d.c})) == "ab");
  test_assert(select(kAdd, "") == "ab" && select(kSet, "").empty());
  /* ALL and NONE don't read it. */
  test_assert(select(kAll, "nonsense") == "abcef" && select(kNone, "nonsense").empty());

  d.obj(d.b).flags = ObjectFlags::Unselectable;
  test_assert(select(kSet, ids(d, {d.a, d.b})) == "a");
  const PropertyBag selectOut = d.ok(kSelectSceneCommand,
                                     {{"mode", PropValue::ofEnum(kSet)},
                                      {"objects", text(ids(d, {d.b, d.a, d.b, d.c}))}});
  test_assert(selectOut.get("skipped")->asInt() == 1);
  test_assert(select(kRemove, ids(d, {d.a, d.b, d.c})).empty());
  test_assert(isError(d.fails(kSelectSceneCommand,
                              {{"object", d.ptr(d.a)}, {"objects", text(ids(d, {d.a}))}}),
                      StatusCode::InvalidArgument,
                      "both"));
}

/* What the gesture command gets at its start, and parents named in the same list. */
void testOthers()
{
  Five d;
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragObjectDeltaCommand);
  PropertyBag in = def->newInputs();
  must(in.set("objects", text(ids(d, {d.e, d.a}))));
  must(d.ex->beginInteractive(*d.ctx, *def, std::move(in)));
  TransformDelta delta;
  delta.basis = quatd(0, 0, 0, 1);
  delta.vector = double3(10, 0, 0);
  must(d.ex->interactiveCommand()->as<ApplyDelta>()->update(*d.ctx, delta));
  must(d.ex->commit(*d.ctx));
  d.evaluate();
  test_assert(d.isAt(d.a, 11) && d.isAt(d.e, 14) && d.isAt(d.f, 5));
  d.undo();
  d.evaluate();
  test_assert(d.isAt(d.a, 1) && d.isAt(d.e, 4));

  test_assert(
      isError(d.fails(kSetObjectParentCommand,
                      {{"parent", d.ptr(d.c)}, {"objects", text(ids(d, {d.e, d.a}))}}),
              StatusCode::FailedPrecondition,
              "'c': is below one of the objects"));
  d.cycle(kSetObjectParentCommand,
          {{"parent", d.ptr(d.c)}, {"objects", text(ids(d, {d.e}))}});
  test_assert(d.lib->resolveRef(d.obj(d.e).parent.cell()) == d.c && selected(d) == "f");

  /* As an invocation's text. */
  const std::string call =
      "core.object.applyDelta(vector=(0, 0, 7) objects=\"" + ids(d, {d.f}) + "\")";
  must(invoke(*d.ctx, call).status());
  d.evaluate();
  test_assert(d.isAt(d.f, 5, 0, 7) && d.isAt(d.a, 1));
  /* The next one is about the selection again, though it gets the vector. */
  d.select({d.a});
  must(invoke(*d.ctx, "core.object.applyDelta").status());
  d.evaluate();
  test_assert(d.isAt(d.f, 5, 0, 7) && d.isAt(d.a, 1, 0, 7));
}

} // namespace

int main()
{
  testSameAsSelected();
  testRefusals();
  testSelect();
  testOthers();
  return test_end();
}

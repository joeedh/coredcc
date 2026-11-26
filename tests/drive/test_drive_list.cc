/* The document's list of driver graphs (drive/driver_list.h): the list, its commands with
 * undo, files, duplicates, more than one list, and the datapath. */
#include "drive_doc.h"

#include "coredcc/api/resolve.h"
#include "coredcc/drive/commands.h"
#include "coredcc/lib/duplicate.h"

test_init;

using namespace drive_test;

namespace {

bool hasCode(const Status &s, StatusCode code)
{
  return s.code() == code;
}

void testList()
{
  DriveDoc d;
  const BlockHandle g1 = d.graph("g1", false), g2 = d.graph("g2", false),
                    g3 = d.graph("g3", false);
  DriverList &l = d.list();
  test_assert(l.graphs(*d.lib).size() == 0);
  must(l.insert(*d.lib, d.idOf(g1)));
  must(l.insert(*d.lib, d.idOf(g2)));
  must(l.insert(*d.lib, d.idOf(g3), 0));
  test_assert(hasCode(l.insert(*d.lib, d.idOf(g2)), StatusCode::AlreadyExists));
  test_assert(hasCode(l.insert(*d.lib, BlockId()), StatusCode::NotFound));
  const Vector<BlockHandle> &live = l.graphs(*d.lib);
  test_assert(live.size() == 3 && live[0] == g3 && live[1] == g1 && live[2] == g2);
  must(d.lib->markDeleted(g1));
  test_assert(l.graphs(*d.lib).size() == 2 && l.cells().size() == 3);
  must(d.lib->restoreFromHistory(g1));
  Result<int> at = l.remove(*d.lib, d.idOf(g1));
  test_assert(at.ok() && *at == 1 && l.graphs(*d.lib).size() == 2);
  test_assert(hasCode(l.remove(*d.lib, d.idOf(g1)).status(), StatusCode::NotFound));

  /* The list is a root, and its cells keep the graphs alive. */
  must(d.lib->collect().status());
  test_assert(d.lib->get(g2) && d.lib->get(g3) && !d.lib->get(g1));
  must(d.lib->validate());
}

/* core.driver.add makes the list, and undo removes it again; core.driver.remove takes the
 * graph out of every list, and undo brings back each cell at its place. */
void testCommands()
{
  DriveDoc d;
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g", false), h = d.graph("h", false);
  d.driveLocation(g, a, 5);
  test_assert(DriverList::find(*d.lib).isNull());
  d.ok(kAddDriverCommand, {{"graph", d.ptr(g)}});
  const BlockHandle made = DriverList::find(*d.lib);
  test_assert(!made.isNull() && d.lib->get(made)->hasFlag(BlockFlags::Root));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));
  must(d.ex->undo(*d.ctx));
  test_assert(DriverList::find(*d.lib).isNull());
  d.evaluate();
  test_assert(d.evaluatedAt(a, 0));
  must(d.ex->redo(*d.ctx));
  test_assert(DriverList::find(*d.lib) == made);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));
  test_assert(d.fails(kAddDriverCommand, {{"graph", d.ptr(a)}}).code() ==
              StatusCode::InvalidArgument);

  /* A second list, as an append brings: both are read; the commands see both. */
  Result<BlockHandle> second = newDriverList(*d.lib);
  must(second.status());
  d.lib->setFlag(*second, BlockFlags::Root, false);
  DriverList &other = *DriverList::of(*d.lib, *second);
  must(other.insert(*d.lib, d.idOf(h)));
  must(other.insert(*d.lib, d.idOf(g)));
  test_assert(DriverList::find(*d.lib) == made); /* the Root one */
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5) && !d.hasDiagnostic("driven twice"));
  test_assert(d.fails(kAddDriverCommand, {{"graph", d.ptr(g)}}).code() ==
              StatusCode::AlreadyExists);

  DriverList &first = *DriverList::of(*d.lib, made);
  must(first.insertCell(*d.lib, d.idOf(h), 0));
  must(first.insertCell(*d.lib, d.idOf(g), 0)); /* g twice: [g, h, g] */
  d.ok(kRemoveDriverCommand, {{"graph", d.ptr(g)}});
  test_assert(first.cells().size() == 1 && other.cells().size() == 1);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 0));
  test_assert(d.fails(kRemoveDriverCommand, {{"graph", d.ptr(g)}}).code() ==
              StatusCode::NotFound);
  must(d.ex->undo(*d.ctx));
  test_assert(first.cells().size() == 3 && other.cells().size() == 2);
  test_assert(first.cells()[0].target == d.idOf(g) &&
              first.cells()[2].target == d.idOf(g));
  test_assert(other.cells()[1].target == d.idOf(g));
  d.evaluate();
  test_assert(d.evaluatedAt(a, 5));
  must(d.ex->redo(*d.ctx));
  test_assert(first.cells().size() == 1 && other.cells().size() == 1);
}

void testFiles()
{
  DriveDoc d;
  const BlockHandle g1 = d.graph("g1", false), g2 = d.graph("g2", false);
  must(d.list().insert(*d.lib, d.idOf(g2)));
  must(d.list().insert(*d.lib, d.idOf(g1)));
  {
    Loaded l(d, save(*d.lib));
    const DriverList *list = DriverList::of(l.lib, DriverList::find(l.lib));
    test_assert(list && list->graphs(l.lib).size() == 2);
    test_assert(list->graphs(l.lib)[0] == l.byName("g2") &&
                list->graphs(l.lib)[1] == l.byName("g1"));
    test_assert(l.lib.get(DriverList::find(l.lib))->hasFlag(BlockFlags::Root));
  }
  /* A deleted graph is written as nothing, and the load drops it. */
  must(d.lib->markDeleted(g1));
  {
    Loaded l(d, save(*d.lib));
    test_assert(DriverList::of(l.lib, DriverList::find(l.lib))->cells().size() == 1);
  }
}

/* Probe P3 of the review: duplicating the scene with its graph doesn't drive the object
 * twice. The scene has no list; a copy of the list is empty. */
void testDuplicates()
{
  DriveDoc d;
  const BlockHandle a = d.object("a");
  const BlockHandle g = d.graph("g");
  d.driveLocation(g, a, 7);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7));
  const BlockHandle both[] = {d.scene, g};
  must(duplicateBlocks(*d.lib, std::span<const BlockHandle>(both, 2)).status());
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7) && d.statusOf(a) == EvalStatus::Ok);

  const BlockHandle list = DriverList::find(*d.lib);
  const BlockHandle withGraph[] = {list, g};
  Result<IdMap> ids = duplicateBlocks(*d.lib, std::span<const BlockHandle>(withGraph, 2));
  must(ids.status());
  test_assert(DriverList::find(*d.lib) == list);
  d.evaluate();
  test_assert(d.evaluatedAt(a, 7) && d.statusOf(a) == EvalStatus::Ok);
  test_assert(DriverList::allLists(*d.lib).size() == 2);
  test_assert(DriverList::allGraphs(*d.lib).size() == 1);
}

/* Probe P5 of the review: the list is the document's; the graphs drive objects of every
 * scene, and no scene lists graphs. */
void testScenes()
{
  DriveDoc d;
  const BlockHandle other = d.addScene("Other");
  const BlockHandle b = d.loose("b", double3(0, 0, 0));
  must(Scene::of(*d.lib, other)->add(*d.lib, b));
  const BlockHandle g = d.graph("g");
  d.driveLocation(g, b, 3);
  must(evaluateScene(*d.ctx, other, nullptr));
  ObjectEval e;
  test_assert(Scene::of(*d.lib, other)->evaluated(b, e) && at(e.state)[0] == 3);
  test_assert(!getValue(*d.ctx, "scene.graphs.length").ok());
}

/* The root collection `driverGraphs`: every list's graphs, each once. */
void testPath()
{
  DriveDoc d;
  ApiRegistry &api = d.rt.ext<ApiRegistry>();
  ApiStruct root("test.DriveRoot");
  addDriverGraphsMember(root);
  must(api.add(std::move(root)).status());
  int owner = 0;
  must(api.addRuntimeOwner("driveRoot",
                           StructId::of("test.DriveRoot"),
                           [&owner]() -> void * { return &owner; })
           .status());
  Context c(&d.rt, d.lib.get(), api.runtimeOwner("driveRoot"));
  Result<PropValue> n = getValue(c, "driverGraphs.length");
  test_assert(n.ok() && n->asInt() == 0);
  const BlockHandle g1 = d.graph("g1"), g2 = d.graph("g2");
  (void)g1;
  n = getValue(c, "driverGraphs.length");
  test_assert(n.ok() && n->asInt() == 2);
  test_assert(resolve(c, "driverGraphs[\"g2\"]").ok());
  test_assert(d.lib->get(g2));
}

} // namespace

int main()
{
  testList();
  testCommands();
  testFiles();
  testDuplicates();
  testScenes();
  testPath();
  return test_end();
}

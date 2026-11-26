/* The facades for scripts (bind/drive_facades.h), natively: tests/wasm/drive.test.ts has
 * them from TS. */
#include "drive_doc.h"

#include "coredcc/bind/drive_facades.h"

#include <string>
#include <vector>

test_init;

using namespace drive_test;
namespace sc = coredcc::script;

int main()
{
  {
    DriveDoc d(false);
    test_assert(sc::driverDiagnostics(*d.ctx).status.code() ==
                StatusCode::FailedPrecondition);
    must(sc::enableDrivers(*d.ctx));
    test_assert(sc::enableDrivers(*d.ctx).code() == StatusCode::AlreadyExists);
    const BlockHandle a = d.object("a");
    const BlockHandle g = d.graph("g");
    d.driveLocation(g, a, 2);
    d.evaluate();
    test_assert(d.evaluatedAt(a, 2));
    sc::StringResult r = sc::driverDiagnostics(*d.ctx);
    test_assert(r.status.ok() && strcmp(r.value.c_str(), "[]") == 0);

    d.driver(g, "float", a, "nowhere");
    r = sc::driverDiagnostics(*d.ctx);
    test_assert(r.status.ok() && strstr(r.value.c_str(), "\"message\":") &&
                strstr(r.value.c_str(), "nowhere"));

    /* Groups nested deeper than 64: the issue is about the last group, with no node. Its
     * path is the groups. */
    std::vector<BlockHandle> chain;
    for (int i = 0; i < 70; i++) {
      chain.push_back(d.graph(("c" + std::to_string(i)).c_str(), false));
    }
    for (int i = 0; i + 1 < 70; i++) {
      const NodeId q = d.node(chain[i], kGraphGroupType);
      must(d.def(chain[i]).setGroup(q, d.idOf(chain[i + 1])));
    }
    must(d.def(g).setGroup(d.node(g, kGraphGroupType), d.idOf(chain[0])));
    bool deep = false;
    for (const DriverDiagnostic &dg : SceneDrivers::of(*d.lib)->diagnostics()) {
      deep = deep || (dg.node.node.isNull() && dg.node.groups.size() > 0);
    }
    test_assert(deep);
    r = sc::driverDiagnostics(*d.ctx);
    test_assert(r.status.ok() && strstr(r.value.c_str(), "deeper than 64") &&
                !strstr(r.value.c_str(), "\"node\":\"\""));
    test_assert(strstr(r.value.c_str(), "\"graph\":\"") &&
                !strstr(r.value.c_str(), "\"graph\":\"\""));

    must(sc::disableDrivers(*d.ctx));
    must(sc::disableDrivers(*d.ctx));
    d.evaluate();
    test_assert(d.evaluatedAt(a, 0));
    /* Enabled again, and left for the Runtime: the Library goes first. */
    must(sc::enableDrivers(*d.ctx));
  }
  {
    /* A host's drivers: scripts see their diagnostics, and can't make a second. */
    DriveDoc d;
    test_assert(sc::enableDrivers(*d.ctx).code() == StatusCode::AlreadyExists);
    test_assert(sc::driverDiagnostics(*d.ctx).status.ok());
  }
  return test_end();
}

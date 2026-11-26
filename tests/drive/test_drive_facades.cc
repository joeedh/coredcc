/* The facades for scripts (bind/drive_facades.h), natively: tests/wasm/drive.test.ts has
 * them from TS. */
#include "drive_doc.h"

#include "coredcc/bind/drive_facades.h"

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

    must(sc::disableDrivers(*d.ctx));
    must(sc::disableDrivers(*d.ctx));
    d.evaluate();
    test_assert(d.evaluatedAt(a, 0));
    /* Enabled again, and left for the Runtime: the Library goes first. */
    must(sc::enableDrivers(*d.ctx));
  }
  return test_end();
}

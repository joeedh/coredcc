/* Loading through a Context: what is notified. */
#include "coredcc/api/library_io.h"

#include "coredcc/api/change_bus.h"

#include "../lib/io_fixture.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

static void test_notifications()
{
  Bytes bytes;
  {
    IoWorld w;
    const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
    w.root(a);
    w.link(a, b);
    w.data(b)->mesh.target = BlockId{kAcme, LocalId{7, 50}};
    w.lib.noteRefsChanged();
    bytes = w.save();
  }
  Knows k;
  k.doc = false;
  IoWorld w(k);
  Context ctx(&w.rt, &w.lib, PointerRef());
  std::vector<BlockId> heard;
  ScopedConnection l = w.rt.ext<ChangeBus>().addListener([&](const OwnerKey *owner) {
    test_assert(owner && owner->kind == OwnerKey::Kind::Block && !w.lib.busy());
    heard.push_back(owner->block);
  });
  Bytes damaged = bytes;
  damaged[50] ^= 1;
  test_assert(!loadLibrary(ctx, view(damaged)).ok() && heard.empty());
  Result<LoadReport> r = loadLibrary(ctx, view(bytes));
  test_assert(r.ok() && r->missingType == 2 && r->missingBlock == 1);
  test_assert(heard.size() == 3);
  for (const BlockHandle h : r->blocks) {
    int times = 0;
    for (const BlockId &id : heard) {
      times += id == w.id(h) ? 1 : 0;
    }
    test_assert(times == 1);
  }
  heard.clear();
  r = resolvePlaceholders(ctx);
  test_assert(r.ok() && heard.empty());
  registerDoc(w.rt, w.docs);
  r = resolvePlaceholders(ctx);
  test_assert(r.ok() && r->blocks.size() == 2 && heard.size() == 2);

  LoadOptions options;
  options.limits.maxBlocks = 2;
  IoWorld w2;
  Context ctx2(&w2.rt, &w2.lib, PointerRef());
  test_assert(loadLibrary(ctx2, view(bytes), options).status().code() == StatusCode::OutOfRange);

  Context none(&w.rt, nullptr, PointerRef());
  test_assert(loadLibrary(none, view(bytes)).status().code() == StatusCode::FailedPrecondition);
  test_assert(resolvePlaceholders(none).status().code() == StatusCode::FailedPrecondition);
}

int main()
{
  test_notifications();
  return test_end();
}

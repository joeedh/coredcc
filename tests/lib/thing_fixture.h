#pragma once

/* A block type for the lib tests: data with one ref of each kind, and a provider that
 * logs what it is asked and can be told to fail. Its state is in a ThingHost, which each
 * test makes, so nothing is global. */

#include "coredcc/base/iface/registry.h"
#include "coredcc/lib/library.h"

#include "litestl/tests/test_util.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace coredcc_test {

using namespace coredcc;
namespace lbt = litestl::binding::types;

struct Thing {
  RefCell owner{RefKind::Strong};
  RefCell material{RefKind::OptStrong};
  RefCell parent{RefKind::Weak};
  int value = 0;
};

/** What the provider does and saw. */
struct ThingHost {
  Runtime *rt = nullptr;
  std::string log;
  /** Set a sticky error from destroy / forEachRef, as a throwing script would. */
  bool failDestroy = false;
  bool failForeach = false;
  /** Called from destroy and from onWeakTargetLost. */
  litestl::util::function<void(const Datablock &)> onDestroy;
  litestl::util::function<void()> onLost;
  /** Called from forEachRef. */
  litestl::util::function<void()> onForeach;
};

struct ThingProvider {
  ThingHost *host = nullptr;

  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void forEachRef(const Datablock &b, RefVisitor &v) const
  {
    if (host->onForeach) {
      host->onForeach();
    }
    if (host->failForeach) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "forEachRef threw"));
      return;
    }
    if (Thing *t = static_cast<Thing *>(b.clientData())) {
      v.visit(t->owner);
      v.visit(t->material);
      v.visit(t->parent);
    }
  }
  void destroy(const Datablock &b)
  {
    host->log += std::string("destroy ") + b.name().c_str() + ";";
    if (host->onDestroy) {
      host->onDestroy(b);
    }
    if (host->failDestroy) {
      host->rt->setStickyError(Status::error(StatusCode::Internal, "destroy threw"));
    }
    if (b.clientData()) {
      litestl::alloc::Delete(static_cast<Thing *>(b.clientData()));
    }
  }
  void onWeakTargetLost(const Datablock &user, const Datablock &lost)
  {
    host->log +=
        std::string("lost ") + user.name().c_str() + "->" + lost.name().c_str() + ";";
    if (host->onLost) {
      host->onLost();
    }
  }
  static lbt::Struct<ThingProvider> *defineBindings()
  {
    auto *st =
        new lbt::Struct<ThingProvider>("test::ThingProvider", sizeof(ThingProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, forEachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, onWeakTargetLost, MARGS("user", "lost"));
    return st;
  }
};

inline const TypeId kThing = TypeId::of("test.Thing");

inline void registerThing(Runtime &rt, ThingHost &host)
{
  host.rt = &rt;
  test_assert(
      (registerImpl<BlockProvider, ThingProvider>(rt, "test.Thing#provider").ok()));
  ThingProvider *p = litestl::alloc::New<ThingProvider>("provider");
  p->host = &host;
  Result<Owned<BlockProvider>> owned = own<BlockProvider>(rt, "test.Thing#provider", p);
  test_assert(owned.ok());
  BlockTypeInfo info;
  info.name = "test.Thing";
  info.client = ClientId::of(kCoredccClient);
  test_assert(rt.ext<BlockTypeRegistry>().add(info, std::move(owned).value()).ok());
}

inline BlockHandle makeThing(Library &lib, const char *name, int value = 0)
{
  Thing *t = litestl::alloc::New<Thing>("Thing");
  t->value = value;
  Result<BlockHandle> h = lib.create(kThing, name, t);
  test_assert(h.ok());
  return h.ok() ? *h : BlockHandle();
}

inline Thing *thing(const Library &lib, BlockHandle h)
{
  return static_cast<Thing *>(lib.get(h)->clientData());
}

inline bool hasMsg(const Status &s, const char *needle)
{
  if (strstr(s.message(), needle)) {
    return true;
  }
  fprintf(stderr, "message was: %s\n", s.message());
  return false;
}

} // namespace coredcc_test

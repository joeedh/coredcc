#include "coredcc/base/runtime.h"

#include "test_util.h"

#include <cstring>
#include <string>

test_init;

using namespace coredcc;

static std::string g_log;

struct ExtA {
  static constexpr const char *kExtName = "test.ExtA";
  Runtime *rt;
  bool broken = false;
  explicit ExtA(Runtime *r) : rt(r)
  {
    g_log += "+A";
  }
  ~ExtA()
  {
    g_log += "-A";
  }
  Status validate() const
  {
    return broken ? Status::error(StatusCode::Internal, "ExtA is broken") : Status();
  }
};

/* Creates ExtA in its constructor, so ExtA must outlive it. */
struct ExtB {
  static constexpr const char *kExtName = "test.ExtB";
  ExtA *a;
  explicit ExtB(Runtime *rt) : a(&rt->ext<ExtA>())
  {
    g_log += "+B";
  }
  ~ExtB()
  {
    g_log += a->rt->findExt<ExtA>() == a ? "-B(A alive)" : "-B(A gone)";
  }
};

static void test_extensions()
{
  g_log.clear();
  {
    Runtime rt;
    test_assert(rt.findExt<ExtB>() == nullptr);
    ExtB &b = rt.ext<ExtB>();
    test_assert(&rt.ext<ExtB>() == &b);
    test_assert(rt.findExt<ExtA>() == b.a);
    test_assert(rt.validateAll().ok());
    b.a->broken = true;
    Status s = rt.validateAll();
    test_assert(!s.ok() && strstr(s.message(), "ExtA is broken") != nullptr);
    b.a->broken = false;

    Runtime other;
    test_assert(other.findExt<ExtA>() == nullptr);
  }
  test_assert(g_log == "+A+B-B(A alive)-A");
}

static void test_new_domains()
{
  static_assert(idDomainTag(IdDomain::Struct) == "struct:");
  static_assert(idDomainTag(IdDomain::Command) == "command:");
  static_assert(StructId::of("x") != StructId{TypeId::of("x").value});
  Runtime rt;
  test_assert(rt.registerId<IdDomain::Struct>("coredcc.Mesh").ok());
  test_assert(rt.registerId<IdDomain::Command>("mesh.extrude").ok());
  test_assert(strcmp(rt.nameOf(CommandId::of("mesh.extrude")), "mesh.extrude") == 0);
  test_assert(rt.nameOf(IdDomain::Type, CommandId::of("mesh.extrude").value) == nullptr);
}

int main()
{
  {
    Runtime rt;
    Result<TypeId> a = rt.registerId<IdDomain::Type>("coredcc.Object");
    test_assert(a.ok() && *a == TypeId::of("coredcc.Object"));
    test_assert(strcmp(rt.nameOf(*a), "coredcc.Object") == 0);

    // Registering the same name again is fine.
    Result<TypeId> again = rt.registerId<IdDomain::Type>("coredcc.Object");
    test_assert(again.ok() && *again == *a);

    // Domains are separate.
    test_assert(rt.nameOf(IdDomain::Interface, a->value) == nullptr);
    test_assert(rt.registerId<IdDomain::Interface>("coredcc.Object").ok());

    // A different name on the same id is a collision naming both.
    Result<uint64_t> clash =
        rt.debugRegisterNameAs(IdDomain::Type, a->value, "someone.else.Object");
    test_assert(!clash.ok());
    test_assert(clash.status().code() == StatusCode::AlreadyExists);
    test_assert(strstr(clash.status().message(), "coredcc.Object") != nullptr);
    test_assert(strstr(clash.status().message(), "someone.else.Object") != nullptr);

    test_assert(!rt.registerName(IdDomain::Type, "").ok());
    test_assert(!rt.debugRegisterNameAs(IdDomain::Type, 0, "zero").ok());

    // Long names (beyond the string's inline capacity) round-trip.
    const char *longName = "a.very.long.client.name.that.exceeds.the.inline.string.capacity";
    Result<ClientId> c = rt.registerId<IdDomain::Client>(longName);
    test_assert(c.ok() && strcmp(rt.nameOf(*c), longName) == 0);
  }
  {
    // Two Runtimes are independent.
    Runtime r1, r2;
    test_assert(r1.registerId<IdDomain::Type>("t").ok());
    test_assert(r2.nameOf(IdDomain::Type, TypeId::of("t").value) == nullptr);
  }

  test_extensions();
  test_new_domains();
  return test_end();
}

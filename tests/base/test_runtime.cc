#include "coredcc/base/runtime.h"

#include "test_util.h"

#include <cstring>

test_init;

using namespace coredcc;

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

  return test_end();
}

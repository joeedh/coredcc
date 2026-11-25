#include "coredcc/base/id.h"

#include "test_util.h"

test_init;

using namespace coredcc;

// Ids are compile-time constants and usable as template arguments.
template <uint64_t V> struct IdHolder {
  static constexpr uint64_t value = V;
};
static_assert(IdHolder<TypeId::of("coredcc.Object").value>::value != 0);

// FNV-1a 64 reference values; these pin the algorithm, which is part of the file format.
static_assert(detail::fnv1a64("") == 0xcbf29ce484222325ull);
static_assert(detail::fnv1a64("a") == 0xaf63dc4c8601ec8cull);
static_assert(detail::fnv1a64("foobar") == 0x85944171f73967e8ull);

// The domain tag is hashed before the name, so domains never share ids.
static_assert(hashId(IdDomain::Type, "x") == detail::fnv1a64("type:x"));
static_assert(TypeId::of("coredcc.Spatial").value !=
              InterfaceId::of("coredcc.Spatial").value);
static_assert(InterfaceId::of("coredcc.Spatial").value !=
              ClientId::of("coredcc.Spatial").value);

int main()
{
  TypeId a = TypeId::of("coredcc.Object");
  TypeId b = TypeId::of("coredcc.Object");
  TypeId c = TypeId::of("coredcc.Scene");
  test_assert(a == b);
  test_assert(a != c);
  test_assert(!a.isNull() && bool(a));
  test_assert(TypeId().isNull() && !TypeId());
  test_assert(a.computeHash() == a.value);
  return test_end();
}

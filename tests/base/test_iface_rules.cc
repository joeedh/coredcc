/* Interface registration rules and edge cases (the interfaces feature report). */
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"

#include "litestl/tests/test_util.h"

#include <cstring>
#include <string>

test_init;

using namespace coredcc;
using litestl::alloc::PermanentGuard;
using litestl::util::string;
namespace lbt = litestl::binding::types;

static void expectValid(const Runtime &rt)
{
  Status s = rt.validateAll();
  if (!s.ok()) {
    fprintf(stderr, "%s\n", s.message());
  }
  test_assert(s.ok());
}

static bool contains(const Status &s, const char *needle)
{
  if (strstr(s.message(), needle)) {
    return true;
  }
  fprintf(stderr, "message was: %s\n", s.message());
  return false;
}

/* ---- value types ---- */

struct Ray {
  float origin = 0.0f;
  static lbt::Struct<Ray> *defineBindings()
  {
    static lbt::Struct<Ray> *st = nullptr;
    if (!st) {
      PermanentGuard guard;
      st = new lbt::Struct<Ray>("rules::Ray", sizeof(Ray));
      BIND_STRUCT_MEMBER(st, origin);
    }
    return st;
  }
};

struct Hit {
  float t = -1.0f;
  int id = 0;
  static lbt::Struct<Hit> *defineBindings()
  {
    static lbt::Struct<Hit> *st = nullptr;
    if (!st) {
      PermanentGuard guard;
      st = new lbt::Struct<Hit>("rules::Hit", sizeof(Hit));
      BIND_STRUCT_MEMBER(st, t);
      BIND_STRUCT_MEMBER(st, id);
    }
    return st;
  }
};

/* Two enums bound under one name with different widths. */
enum class Mode8 : uint8_t { A = 0, B = 1 };
enum class Mode64 : int64_t { A = 0, B = 1 };
template <> struct litestl::binding::Binder<Mode8> {
  static const BindingBase *bind()
  {
    PermanentGuard guard;
    static lbt::Enum *e = new lbt::Enum("rules::Mode", sizeof(Mode8));
    return e;
  }
};
template <> struct litestl::binding::Binder<Mode64> {
  static const BindingBase *bind()
  {
    PermanentGuard guard;
    static lbt::Enum *e = new lbt::Enum("rules::Mode", sizeof(Mode64));
    return e;
  }
};

/* ---- interfaces ---- */

struct Counter : IfaceRef {
  int count() const
  {
    return CDCC_CALL(count);
  }
  double cost() const
  {
    return CDCC_HAS(cost) ? CDCC_CALL(cost) : 1.0;
  }
  void bump(int by)
  {
    CDCC_CALL(bump, by);
  }
  CDCC_INTERFACE(Counter, "rules.Counter")
  {
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_OPTIONAL_METHOD(st, cost, MARGS());
    BIND_STRUCT_METHOD(st, bump, MARGS("by"));
  }
};

/* Non-trivial returns. */
struct Probe : IfaceRef {
  string name() const
  {
    return CDCC_CALL(name);
  }
  Hit probe(const Ray &ray) const
  {
    return CDCC_CALL(probe, ray);
  }
  CDCC_INTERFACE(Probe, "rules.Probe")
  {
    BIND_STRUCT_METHOD(st, name, MARGS());
    BIND_STRUCT_METHOD(st, probe, MARGS("ray"));
  }
};

/* Three levels of concatenation, plus an upcast base at the bottom. */
struct Named : IfaceRef {
  int nameLength() const
  {
    return CDCC_CALL(nameLength);
  }
  CDCC_INTERFACE(Named, "rules.Named")
  {
    BIND_STRUCT_METHOD(st, nameLength, MARGS());
  }
};
struct LevelA : IfaceRef {
  int a() const
  {
    return CDCC_CALL(a);
  }
  CDCC_INTERFACE(LevelA, "rules.LevelA")
  {
    BIND_STRUCT_METHOD(st, a, MARGS());
  }
};
struct LevelB : LevelA {
  int b() const
  {
    return CDCC_CALL(b);
  }
  CDCC_INTERFACE_EXTENDS(LevelB, "rules.LevelB", LevelA)
  {
    BIND_STRUCT_METHOD(st, b, MARGS());
  }
};
struct LevelC : LevelB {
  int c() const
  {
    return CDCC_CALL(c);
  }
  CDCC_UPCASTS(Named);
  CDCC_INTERFACE_EXTENDS(LevelC, "rules.LevelC", LevelB)
  {
    BIND_STRUCT_METHOD(st, c, MARGS());
  }
};

struct Moded : IfaceRef {
  int pick(Mode8 m) const
  {
    return CDCC_CALL(pick, m);
  }
  CDCC_INTERFACE(Moded, "rules.Moded")
  {
    BIND_STRUCT_METHOD(st, pick, MARGS("m"));
  }
};

struct Taker : IfaceRef {
  int take(const string &s) const
  {
    return CDCC_CALL(take, s);
  }
  CDCC_INTERFACE(Taker, "rules.Taker")
  {
    BIND_STRUCT_METHOD(st, take, MARGS("s"));
  }
};

/* Declaration-rule violations, one interface each. Their forwarders are never called. */
struct Overloaded : IfaceRef {
  int f(int) const
  {
    return 0;
  }
  int f(float) const
  {
    return 0;
  }
  CDCC_INTERFACE(Overloaded, "rules.Overloaded")
  {
    BIND_STRUCT_METHOD_SIG(st, f, int, MARGS("x"), (int) const);
    BIND_STRUCT_METHOD_SIG(st, f, int, MARGS("x"), (float) const);
  }
};
struct RefReturn : IfaceRef {
  Ray &ray() const
  {
    static Ray r;
    return r;
  }
  CDCC_INTERFACE(RefReturn, "rules.RefReturn")
  {
    BIND_STRUCT_METHOD(st, ray, MARGS());
  }
};
struct OutParam : IfaceRef {
  void read(int &out) const
  {
    out = 0;
  }
  CDCC_INTERFACE(OutParam, "rules.OutParam")
  {
    BIND_STRUCT_METHOD(st, read, MARGS("out"));
  }
};
struct ConstPtrParam : IfaceRef {
  int read(const int *in) const
  {
    return in ? *in : 0;
  }
  CDCC_INTERFACE(ConstPtrParam, "rules.ConstPtrParam")
  {
    BIND_STRUCT_METHOD(st, read, MARGS("in"));
  }
};
struct Reserved : IfaceRef {
  int as() const
  {
    return 0;
  }
  CDCC_INTERFACE(Reserved, "rules.Reserved")
  {
    BIND_STRUCT_METHOD(st, as, MARGS());
  }
};
struct StaticSlot : IfaceRef {
  static int make()
  {
    return 0;
  }
  CDCC_INTERFACE(StaticSlot, "rules.StaticSlot")
  {
    BIND_STRUCT_STATIC_METHOD(st, make, MARGS());
  }
};
/* Same name as Counter, different C++ interface. */
struct CounterTwin : IfaceRef {
  int count() const
  {
    return CDCC_CALL(count);
  }
  CDCC_INTERFACE(CounterTwin, "rules.Counter")
  {
    BIND_STRUCT_METHOD(st, count, MARGS());
  }
};

/* ---- implementations ---- */

struct Tally {
  int n = 3;
  int count() const
  {
    return n;
  }
  void bump(int by)
  {
    n += by;
  }
  static lbt::Struct<Tally> *defineBindings()
  {
    auto *st = new lbt::Struct<Tally>("rules::Tally", sizeof(Tally));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, bump, MARGS("by"));
    return st;
  }
};

struct WrongCost {
  int count() const
  {
    return 1;
  }
  int cost() const /* optional slot present, wrong return type */
  {
    return 2;
  }
  void bump(int)
  {
  }
  static lbt::Struct<WrongCost> *defineBindings()
  {
    auto *st = new lbt::Struct<WrongCost>("rules::WrongCost", sizeof(WrongCost));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, cost, MARGS());
    BIND_STRUCT_METHOD(st, bump, MARGS("by"));
    return st;
  }
};

struct Prober {
  string label = "a long enough label to need the heap, past any small buffer";
  string name() const
  {
    return label;
  }
  Hit probe(const Ray &ray) const
  {
    return Hit{ray.origin + 1.0f, 7};
  }
  static lbt::Struct<Prober> *defineBindings()
  {
    auto *st = new lbt::Struct<Prober>("rules::Prober", sizeof(Prober));
    BIND_STRUCT_METHOD(st, name, MARGS());
    BIND_STRUCT_METHOD(st, probe, MARGS("ray"));
    return st;
  }
};

struct Leveled {
  int a() const
  {
    return 1;
  }
  int b() const
  {
    return 2;
  }
  int c() const
  {
    return 3;
  }
  int nameLength() const
  {
    return 4;
  }
  static lbt::Struct<Leveled> *defineBindings()
  {
    auto *st = new lbt::Struct<Leveled>("rules::Leveled", sizeof(Leveled));
    BIND_STRUCT_METHOD(st, a, MARGS());
    BIND_STRUCT_METHOD(st, b, MARGS());
    BIND_STRUCT_METHOD(st, c, MARGS());
    BIND_STRUCT_METHOD(st, nameLength, MARGS());
    return st;
  }
};

/* Satisfies LevelC but not its upcast base Named. */
struct NoName {
  int a() const
  {
    return 1;
  }
  int b() const
  {
    return 2;
  }
  int c() const
  {
    return 3;
  }
  static lbt::Struct<NoName> *defineBindings()
  {
    auto *st = new lbt::Struct<NoName>("rules::NoName", sizeof(NoName));
    BIND_STRUCT_METHOD(st, a, MARGS());
    BIND_STRUCT_METHOD(st, b, MARGS());
    BIND_STRUCT_METHOD(st, c, MARGS());
    return st;
  }
};

struct WideModes {
  int pick(Mode64 m) const
  {
    return int(m);
  }
  static lbt::Struct<WideModes> *defineBindings()
  {
    auto *st = new lbt::Struct<WideModes>("rules::WideModes", sizeof(WideModes));
    BIND_STRUCT_METHOD(st, pick, MARGS("m"));
    return st;
  }
};

struct Stealer {
  int take(string &&s) const
  {
    string mine = std::move(s);
    return int(mine.size());
  }
  static lbt::Struct<Stealer> *defineBindings()
  {
    auto *st = new lbt::Struct<Stealer>("rules::Stealer", sizeof(Stealer));
    BIND_STRUCT_METHOD(st, take, MARGS("s"));
    return st;
  }
};

/* ---- fake foreign host ---- */

struct FakeHost : ForeignDispatcher {
  bool failNext = false;
  std::string log;
  Status
  call(const IfaceTable &table, void *self, int slot, void **args, void *ret) override
  {
    const lbt::Method *m = table.ifaceDesc->methods[slot];
    log += std::string(m->name.c_str()) + ";";
    if (failNext) {
      failNext = false;
      return Status::error(StatusCode::Internal, "TypeError: not a function");
    }
    const char *n = m->name.c_str();
    int base = *static_cast<int *>(self);
    if (strcmp(n, "name") == 0) {
      new (ret) string("from js");
    } else if (strcmp(n, "probe") == 0) {
      new (ret) Hit{static_cast<const Ray *>(args[0])->origin * 2.0f, base};
    } else if (strcmp(n, "count") == 0 || strcmp(n, "a") == 0) {
      new (ret) int(base);
    } else if (strcmp(n, "cost") == 0) {
      new (ret) double(base * 10.0);
    } else if (strcmp(n, "b") == 0 || strcmp(n, "c") == 0 || strcmp(n, "nameLength") == 0)
    {
      new (ret) int(base + 1);
    }
    return Status();
  }
};

/* ---- tests ---- */

static void test_declaration_rules()
{
  Runtime rt;
  test_assert(contains(registerInterface<Overloaded>(rt), "slot 'f' is overloaded"));
  test_assert(
      contains(registerInterface<RefReturn>(rt), "slot 'ray' returns a reference"));
  test_assert(contains(registerInterface<OutParam>(rt), "primitive out-parameter 'out'"));
  test_assert(contains(registerInterface<Reserved>(rt), "reserved by IfaceRef"));
  test_assert(contains(registerInterface<StaticSlot>(rt), "slot 'make' is static"));
  test_assert(registerInterface<ConstPtrParam>(rt).ok()); /* const int* is an input */
  test_assert(registerInterface<Counter>(rt).ok());
  test_assert(registerInterface<Counter>(rt).ok()); /* idempotent */
  Status twin = registerInterface<CounterTwin>(rt);
  test_assert(twin.code() == StatusCode::AlreadyExists);
  test_assert(contains(twin, "registered twice"));
  expectValid(rt);
}

static void test_nontrivial_returns()
{
  Runtime rt;
  test_assert((registerImpl<Probe, Prober>(rt, "rules.Prober").ok()));
  Prober prober;
  Probe p = query<Probe>(rt, TypeId::of("rules.Prober"), &prober);
  test_assert(p.name() == prober.label);
  const Ray ray{2.0f};
  Hit hit = p.probe(ray);
  test_assert(hit.t == 3.0f && hit.id == 7);

  FakeHost host;
  const char *slots[] = {"name", "probe"};
  test_assert(registerForeign<Probe>(rt, "rules.JsProbe", &host, nullptr, slots).ok());
  int handle = 5;
  Probe js = query<Probe>(rt, TypeId::of("rules.JsProbe"), &handle);
  test_assert(strcmp(js.name().c_str(), "from js") == 0);
  hit = js.probe(ray);
  test_assert(hit.t == 4.0f && hit.id == 5);

  /* A failed foreign call: a value-initialized result and a sticky error. */
  test_assert(!rt.hasStickyError());
  host.failNext = true;
  string failed = js.name();
  test_assert(failed.size() == 0);
  test_assert(rt.hasStickyError());
  Status sticky = rt.takeStickyError();
  test_assert(contains(sticky, "TypeError"));
  test_assert(!rt.hasStickyError());
  expectValid(rt);
}

static void test_base_queries()
{
  Runtime rt;
  test_assert((registerImpl<LevelC, Leveled>(rt, "rules.Leveled").ok()));
  test_assert(rt.ifaces().tableCount() == 4); /* C, B, A and Named */
  Leveled obj;
  const TypeId type = TypeId::of("rules.Leveled");
  LevelC c = query<LevelC>(rt, type, &obj);
  test_assert(c.a() == 1 && c.b() == 2 && c.c() == 3);
  test_assert(query<LevelA>(rt, type, &obj).a() == 1);
  test_assert(query<LevelB>(rt, type, &obj).b() == 2);
  test_assert(upcast<LevelA>(c).a() == 1);
  test_assert(upcast<LevelC>(c).c() == 3);
  test_assert(upcast<Named>(c).nameLength() == 4);
  /* From a base back to the others. */
  Named named = upcast<Named>(c);
  test_assert(upcast<LevelB>(named).b() == 2);
  test_assert(!upcast<Counter>(c));
  expectValid(rt);
}

static void test_wrong_types()
{
  Runtime rt;
  Status wide = registerImpl<Moded, WideModes>(rt, "rules.WideModes");
  test_assert(wide.code() == StatusCode::TypeMismatch);
  test_assert(contains(wide, "argument 1 ('m')"));

  Status steal = registerImpl<Taker, Stealer>(rt, "rules.Stealer");
  test_assert(steal.code() == StatusCode::TypeMismatch);
  test_assert(contains(steal, "&&, slot wants"));

  test_assert((registerImpl<Counter, Tally>(rt, "rules.Thing").ok()));
  Status other = registerImpl<Probe, Prober>(rt, "rules.Thing");
  test_assert(other.code() == StatusCode::AlreadyExists);
  test_assert(contains(other, "already bound to another C++ type"));

  /* own() with an object of another type than the registered one. */
  Result<Owned<Counter>> owned =
      own<Counter>(rt, "rules.Thing", litestl::alloc::New<Leveled>("wrong type"));
  test_assert(!owned.ok());
  test_assert(owned.status().code() == StatusCode::TypeMismatch);

  /* A failed registration doesn't poison the name. */
  test_assert((registerImpl<Moded, WideModes>(rt, "rules.Retry").code() ==
               StatusCode::TypeMismatch));
  test_assert((registerImpl<Counter, Tally>(rt, "rules.Retry").ok()));
  expectValid(rt);
}

static void test_optional_slots()
{
  Runtime rt;
  Status s = registerImpl<Counter, WrongCost>(rt, "rules.WrongCost");
  test_assert(s.code() == StatusCode::TypeMismatch);
  test_assert(contains(s, "candidate int32 cost() const: return type int32 != double"));

  FakeHost host;
  const char *withCost[] = {"count", "cost", "bump"};
  test_assert(
      registerForeign<Counter>(rt, "rules.JsCostly", &host, nullptr, withCost).ok());
  int handle = 2;
  Counter c = query<Counter>(rt, TypeId::of("rules.JsCostly"), &handle);
  test_assert(c.cost() == 20.0);
  const char *withoutCost[] = {"count", "bump"};
  test_assert(
      registerForeign<Counter>(rt, "rules.JsCheap", &host, nullptr, withoutCost).ok());
  Counter cheap = query<Counter>(rt, TypeId::of("rules.JsCheap"), &handle);
  host.log.clear();
  test_assert(cheap.cost() == 1.0);
  test_assert(host.log.empty());
  expectValid(rt);
}

static void test_rollback()
{
  Runtime rt;
  Status s = registerImpl<LevelC, NoName>(rt, "rules.NoName");
  test_assert(s.code() == StatusCode::TypeMismatch);
  test_assert(contains(s, "does not implement rules.Named"));
  test_assert(rt.ifaces().tableCount() == 0);
  NoName obj;
  test_assert(!query<LevelC>(rt, TypeId::of("rules.NoName"), &obj));

  /* Named registered first: the LevelC registration reuses that table. */
  test_assert((registerImpl<Named, Leveled>(rt, "rules.Leveled").ok()));
  test_assert((registerImpl<LevelC, Leveled>(rt, "rules.Leveled").ok()));
  test_assert(rt.ifaces().tableCount() == 4);
  Leveled lev;
  LevelC c = query<LevelC>(rt, TypeId::of("rules.Leveled"), &lev);
  test_assert(upcast<Named>(c).table() ==
              query<Named>(rt, TypeId::of("rules.Leveled"), &lev).table());
  expectValid(rt);
}

static void test_foreign_family()
{
  Runtime rt;
  FakeHost host;
  const char *slots[] = {"a", "b", "c", "nameLength"};
  test_assert(registerForeign<LevelC>(rt, "rules.JsLevels", &host, nullptr, slots).ok());
  int handle = 10;
  const TypeId type = TypeId::of("rules.JsLevels");
  test_assert(query<LevelA>(rt, type, &handle).a() == 10);
  test_assert(query<Named>(rt, type, &handle).nameLength() == 11);
  LevelC c = query<LevelC>(rt, type, &handle);
  test_assert(upcast<Named>(c).nameLength() == 11);
  test_assert(hasFlag(upcast<Named>(c).table()->flags, IfaceFlags::ThreadAffine));

  /* The untyped entry point registers the type name too. */
  const char *countSlots[] = {"count", "bump"};
  test_assert(registerInterface<Counter>(rt).ok());
  test_assert(rt.ifaces()
                  .addForeign(InterfaceId::of("rules.Counter"),
                              "rules.Direct",
                              &host,
                              nullptr,
                              countSlots)
                  .ok());
  test_assert(rt.nameOf(TypeId::of("rules.Direct")) != nullptr);

  const char *missing[] = {"a", "b", "c"};
  Status s = registerForeign<LevelC>(rt, "rules.JsNoName", &host, nullptr, missing);
  test_assert(contains(s, "slot nameLength [int32 nameLength() const]: missing"));
  expectValid(rt);
}

static int g_live = 0;
struct CountedTally : Tally {
  CountedTally()
  {
    g_live++;
  }
  ~CountedTally()
  {
    g_live--;
  }
  static lbt::Struct<CountedTally> *defineBindings()
  {
    auto *st = new lbt::Struct<CountedTally>("rules::CountedTally", sizeof(CountedTally));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, bump, MARGS("by"));
    return st;
  }
};

static Owned<Counter> makeOwned(const Runtime &rt)
{
  Result<Owned<Counter>> r = own<Counter>(
      rt, "rules.CountedTally", litestl::alloc::New<CountedTally>("counted"));
  test_assert(r.ok());
  return std::move(r).value();
}

static void test_owned()
{
  Runtime rt;
  test_assert((registerImpl<Counter, CountedTally>(rt, "rules.CountedTally").ok()));
  {
    Owned<Counter> a = makeOwned(rt);
    Owned<Counter> b = makeOwned(rt);
    test_assert(g_live == 2);
    a->bump(4); /* a mutating slot through Owned */
    test_assert(a->count() == 7);
    a = std::move(b); /* destroys a's object */
    test_assert(g_live == 1);
    test_assert(a->count() == 3);
    Owned<Counter> &alias = a;
    a = std::move(alias); /* self-move keeps it */
    test_assert(g_live == 1 && bool(a));
    a.reset();
    test_assert(g_live == 0 && !a);
  }
  test_assert(g_live == 0);
  expectValid(rt);
}

static void test_two_runtimes()
{
  Tally t;
  {
    Runtime one;
    Runtime two;
    test_assert((
        registerImpl<Counter, Tally>(one, "rules.Tally", IfaceFlags::ThreadAffine).ok()));
    test_assert((registerImpl<Counter, Tally>(two, "rules.Tally").ok()));
    Counter c1 = query<Counter>(one, TypeId::of("rules.Tally"), &t);
    Counter c2 = query<Counter>(two, TypeId::of("rules.Tally"), &t);
    test_assert(c1.table() != c2.table());
    test_assert(hasFlag(c1.table()->flags, IfaceFlags::ThreadAffine));
    test_assert(!hasFlag(c2.table()->flags, IfaceFlags::ThreadAffine));
    expectValid(one);
    expectValid(two);
  }
  Runtime three;
  test_assert((registerImpl<Counter, Tally>(three, "rules.Tally").ok()));
  test_assert(query<Counter>(three, TypeId::of("rules.Tally"), &t).count() == 3);
}

int main()
{
  test_declaration_rules();
  test_nontrivial_returns();
  test_base_queries();
  test_wrong_types();
  test_optional_slots();
  test_rollback();
  test_foreign_family();
  test_owned();
  test_two_runtimes();
  return test_end();
}

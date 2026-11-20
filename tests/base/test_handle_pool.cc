#include "coredcc/base/handle_pool.h"

#include "test_util.h"

test_init;

using namespace coredcc;

static int g_live = 0;

struct Tracked {
  int value;
  explicit Tracked(int v) : value(v)
  {
    g_live++;
  }
  ~Tracked()
  {
    g_live--;
  }
};

int main()
{
  {
    HandlePool<Tracked> pool;
    using H = HandlePool<Tracked>::HandleT;
    test_assert(H().isNull() && !pool.valid(H()));

    H a = pool.create(1);
    H b = pool.create(2);
    test_assert(a.generation == 1 && b.generation == 1);
    test_assert(pool.get(a)->value == 1 && pool.get(b)->value == 2);
    test_assert(pool.size() == 2 && g_live == 2);

    Tracked *pa = pool.get(a);
    test_assert(pool.destroy(a));
    test_assert(!pool.destroy(a));
    test_assert(!pool.valid(a) && pool.get(a) == nullptr);
    test_assert(g_live == 1);

    // The slot is reused with a new generation; the stale handle stays invalid.
    H c = pool.create(3);
    test_assert(c.index == a.index && c.generation == 2);
    test_assert(!pool.valid(a) && pool.get(c)->value == 3);
    test_assert(pool.get(c) == pa); // same storage

    // Pointers stay stable across page growth.
    Tracked *pb = pool.get(b);
    H many[1000];
    for (int i = 0; i < 1000; i++) {
      many[i] = pool.create(100 + i);
    }
    test_assert(pool.get(b) == pb);
    for (int i = 0; i < 1000; i++) {
      test_assert(pool.get(many[i])->value == 100 + i);
    }

    int sum = 0;
    pool.forEach([&](H, Tracked &t) { sum += t.value < 100 ? t.value : 0; });
    test_assert(sum == 2 + 3);
  }
  test_assert(g_live == 0); // the destructor destroys live objects

  {
    // A slot whose generation reaches the maximum is retired, never reused.
    HandlePool<Tracked> pool;
    auto a = pool.create(1);
    test_assert(pool.destroy(a));
    pool.debugSetGeneration(a.index, UINT32_MAX);
    auto b = pool.create(2);
    test_assert(b.index == a.index && b.generation == UINT32_MAX);
    test_assert(pool.destroy(b));
    auto c = pool.create(3);
    test_assert(c.index != b.index);
    test_assert(!pool.valid(b));
  }
  {
    // Quarantine never reuses a slot.
    HandlePool<Tracked> pool;
    pool.setQuarantine(true);
    auto a = pool.create(1);
    test_assert(pool.destroy(a));
    auto b = pool.create(2);
    test_assert(b.index != a.index);
    test_assert(!pool.valid(a) && pool.valid(b));
    pool.clear();
    test_assert(pool.size() == 0 && !pool.valid(b));
  }
  test_assert(g_live == 0);

  return test_end();
}

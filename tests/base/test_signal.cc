#include "coredcc/base/signal.h"

#include "test_util.h"

#include <memory>

test_init;

using namespace coredcc;

int main()
{
  {
    Signal<int> sig;
    int total = 0;
    Connection c1 = sig.connect([&](int v) { total += v; });
    Connection c2 = sig.connect([&](int v) { total += 10 * v; });
    sig.emit(1);
    test_assert(total == 11);
    test_assert(sig.slotCount() == 2 && c1.connected());
    c1.disconnect();
    test_assert(!c1.connected() && c2.connected());
    sig.emit(1);
    test_assert(total == 21);
  }
  {
    // A slot connected during an emit is not called by that emit.
    Signal<> sig;
    int calls = 0;
    sig.connect([&]() {
      calls++;
      if (calls == 1) {
        sig.connect([&]() { calls += 100; });
      }
    });
    sig.emit();
    test_assert(calls == 1);
    sig.emit();
    test_assert(calls == 102);
  }
  {
    // A slot disconnected by another slot during an emit is not called again.
    Signal<> sig;
    int laterCalls = 0;
    Connection later;
    sig.connect([&]() { later.disconnect(); });
    later = sig.connect([&]() { laterCalls++; });
    sig.emit();
    test_assert(laterCalls == 0);
    test_assert(sig.slotCount() == 1);
  }
  {
    // A slot may disconnect itself.
    Signal<> sig;
    int calls = 0;
    Connection self;
    self = sig.connect([&]() {
      calls++;
      self.disconnect();
    });
    sig.emit();
    sig.emit();
    test_assert(calls == 1);
  }
  {
    // Re-entrant emit.
    Signal<int> sig;
    int sum = 0;
    sig.connect([&](int depth) {
      sum += depth;
      if (depth < 3) {
        sig.emit(depth + 1);
      }
    });
    sig.emit(1);
    test_assert(sum == 1 + 2 + 3);
  }
  {
    // A connection outliving its Signal is harmless.
    Connection c;
    {
      Signal<> sig;
      c = sig.connect([]() {});
      test_assert(c.connected());
    }
    test_assert(!c.connected());
    c.disconnect();
  }
  {
    // ScopedConnection disconnects on destruction, and can be moved.
    Signal<> sig;
    int calls = 0;
    {
      ScopedConnection outer;
      {
        ScopedConnection inner = sig.connect([&]() { calls++; });
        outer = std::move(inner);
      }
      sig.emit();
      test_assert(calls == 1);
    }
    sig.emit();
    test_assert(calls == 1 && sig.slotCount() == 0);
  }
  {
    // Captured state is released when the slot is disconnected.
    Signal<> sig;
    auto token = std::make_shared<int>(7);
    std::weak_ptr<int> weak = token;
    Connection c = sig.connect([token]() {});
    token.reset();
    test_assert(!weak.expired());
    c.disconnect();
    test_assert(weak.expired());
  }
  {
    Signal<> sig;
    sig.connect([]() {});
    sig.connect([]() {});
    sig.disconnectAll();
    test_assert(sig.slotCount() == 0);
  }

  return test_end();
}

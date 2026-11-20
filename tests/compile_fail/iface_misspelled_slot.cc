/* CDCC_CALL with a slot name the interface doesn't have must not compile. */
#include "coredcc/base/iface/iface.h"

struct Counted : coredcc::IfaceRef {
  int count() const
  {
#ifdef COREDCC_COMPILE_FAIL
    return CDCC_CALL(cuont);
#else
    return CDCC_CALL(count);
#endif
  }
  CDCC_INTERFACE(Counted, "test.Counted")
  {
    BIND_STRUCT_METHOD(st, count, MARGS());
  }
};

int main()
{
  return Counted::defineBindings() != nullptr ? 0 : 1;
}

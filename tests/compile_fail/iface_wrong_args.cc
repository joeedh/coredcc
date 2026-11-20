/* CDCC_CALL takes its argument types from the forwarder; a mismatch must not compile. */
#include "coredcc/base/iface/iface.h"

struct Scaled : coredcc::IfaceRef {
  int scale(int by, double factor) const
  {
#ifdef COREDCC_COMPILE_FAIL
    return CDCC_CALL(scale, by);
#else
    return CDCC_CALL(scale, by, factor);
#endif
  }
  CDCC_INTERFACE(Scaled, "test.Scaled")
  {
    BIND_STRUCT_METHOD(st, scale, MARGS("by", "factor"));
  }
};

int main()
{
  return Scaled::defineBindings() != nullptr ? 0 : 1;
}

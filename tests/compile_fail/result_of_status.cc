#include "coredcc/base/status.h"

using namespace coredcc;

int main()
{
#ifdef COREDCC_COMPILE_FAIL
  Result<Status> r(Status::error(StatusCode::Internal, "x"));
#else
  Result<int> r(1);
#endif
  return r.ok() ? 0 : 1;
}

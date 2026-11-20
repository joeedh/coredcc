#include "coredcc/base/status.h"

using namespace coredcc;

static Status mayFail()
{
  return Status::error(StatusCode::Internal, "x");
}

int main()
{
#ifdef COREDCC_COMPILE_FAIL
  mayFail();
#else
  mayFail().ignore();
#endif
  return 0;
}

#include "coredcc/base/status.h"

#include "litestl/tests/test_util.h"

#include <cstring>

test_init;

using namespace coredcc;

static Status failIf(bool fail)
{
  if (fail) {
    return Status::error(StatusCode::NotFound, "no such path 'scene.objcts'");
  }
  return Status();
}

static Status chain(bool fail)
{
  CDCC_TRY(failIf(fail));
  return Status::error(StatusCode::Internal, "reached the end");
}

static Result<int> parse(const char *s)
{
  if (!s || !*s) {
    return Status::error(StatusCode::InvalidArgument, "empty");
  }
  return int(strlen(s));
}

int main()
{
  {
    Status ok;
    test_assert(ok.ok());
    test_assert(ok.code() == StatusCode::Ok);
    test_assert(strcmp(ok.message(), "") == 0);
    test_assert(ok.suggestions().size() == 0);
    test_assert(strcmp(ok.toString().c_str(), "Ok") == 0);
    test_assert(sizeof(Status) == sizeof(void *));
  }
  {
    Status e = Status::error(StatusCode::NotFound, "no such path 'scene.objcts'")
                   .suggest("scene.objects")
                   .suggest("scene.object");
    test_assert(!e.ok());
    test_assert(e.code() == StatusCode::NotFound);
    test_assert(e.suggestions().size() == 2);
    test_assert(strcmp(e.toString().c_str(),
                       "NotFound: no such path 'scene.objcts' (did you mean: "
                       "scene.objects, scene.object?)") == 0);

    Status copy = e;
    test_assert(strcmp(copy.toString().c_str(), e.toString().c_str()) == 0);
    Status moved = std::move(copy);
    test_assert(copy.ok()); // NOLINT: moved-from is ok by contract
    test_assert(moved.code() == StatusCode::NotFound);
    moved = Status();
    test_assert(moved.ok());
    Status &alias = e;
    e = alias; // self-assignment keeps the error
    test_assert(e.code() == StatusCode::NotFound);
  }
  {
    Status ok;
    ok.suggest("ignored");
    test_assert(ok.ok() && ok.suggestions().size() == 0);
  }
  {
    test_assert(chain(true).code() == StatusCode::NotFound);
    test_assert(chain(false).code() == StatusCode::Internal);
  }
  {
    Result<int> a = parse("abc");
    test_assert(a.ok() && *a == 3 && a.status().ok());
    Result<int> b = parse("");
    test_assert(!b.ok() && b.status().code() == StatusCode::InvalidArgument);
    test_assert(std::move(b).valueOr(-1) == -1);
  }
  test_assert(strcmp(statusCodeName(StatusCode::Busy), "Busy") == 0);

  return test_end();
}

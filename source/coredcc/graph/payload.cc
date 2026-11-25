#include "coredcc/graph/payload.h"

#include "coredcc/base/assert.h"

#include <functional>
#include <thread>

namespace coredcc::detail {

namespace {

size_t thisThread()
{
#ifdef NDEBUG
  return 0;
#else
  return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

} // namespace

PayloadBox::PayloadBox(TypeId type) : type_(type), thread_(thisThread()) {}

void PayloadBox::checkThread() const
{
  CDCC_ASSERT(thread_ == thisThread(), "a payload reference was used on another thread");
}

void PayloadBox::ref()
{
  checkThread();
  refs_++;
}

void PayloadBox::unref()
{
  checkThread();
  if (--refs_ == 0) {
    destroySelf();
  }
}

} // namespace coredcc::detail

#include "coredcc/graph/payload.h"

#include "coredcc/base/assert.h"
#include "coredcc/lib/registry.h"

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

PayloadBox::PayloadBox(TypeId type, litestl::util::Vector<LibraryPin, 1> pins)
    : type_(type), thread_(thisThread()), pins_(std::move(pins))
{
}

bool PayloadBox::pinsAlive() const
{
  for (const LibraryPin &p : pins_) {
    if (!p.dataAlive()) {
      return false;
    }
  }
  return true;
}

Result<litestl::util::Vector<LibraryPin, 1>> pinAliased(Library &lib,
                                                        std::span<const BlockHandle> blocks)
{
  if (blocks.empty()) {
    return Status::error(StatusCode::InvalidArgument, "makeAlias: no blocks");
  }
  litestl::util::Vector<LibraryPin, 1> pins;
  for (BlockHandle h : blocks) {
    const Datablock *b = lib.get(h);
    if (!b) {
      return Status::error(StatusCode::NotFound, "makeAlias: no such block");
    }
    const std::string name = "makeAlias: block '" + std::string(b->name().c_str()) + "' ";
    if (b->state() != BlockState::Live) {
      return Status::error(StatusCode::FailedPrecondition, (name + "isn't Live").c_str());
    }
    const BlockTypeRegistry::Entry *type =
        lib.runtime().ext<BlockTypeRegistry>().find(b->type());
    if (type && type->info.storage == BlockStorage::Transient) {
      return Status::error(StatusCode::Unsupported, (name + "is of a Transient type").c_str());
    }
    pins.append(LibraryPin(lib, h));
  }
  return pins;
}

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

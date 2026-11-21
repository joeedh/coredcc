#include "coredcc/base/runtime.h"

#include "coredcc/base/iface/registry.h"

#include "litestl/util/alloc.h"

#include <cstdio>
#include <string>
#include <utility>

namespace coredcc {

using litestl::util::string;

Runtime::Runtime() : ifaces_(litestl::alloc::New<IfaceRegistry>("coredcc::IfaceRegistry", this))
{
}

Runtime::~Runtime()
{
  /* Pop one at a time: a destructor may still look up an older extension. */
  while (exts_.size() > 0) {
    ExtSlot slot = exts_.pop_back();
    slot.destroy(slot.obj);
  }
  litestl::alloc::Delete(ifaces_);
}

Status Runtime::validateAll() const
{
  CDCC_TRY(ifaces_->validateAll());
  for (const ExtSlot &slot : exts_) {
    if (slot.validate) {
      CDCC_TRY(slot.validate(slot.obj));
    }
  }
  return Status();
}

void Runtime::setStickyError(Status status)
{
  if (stickyError_.ok()) {
    stickyError_ = std::move(status);
  }
}

Status Runtime::takeStickyError()
{
  return std::exchange(stickyError_, Status());
}

Result<uint64_t> Runtime::registerName(IdDomain domain, std::string_view name)
{
  if (name.empty()) {
    return Status::error(StatusCode::InvalidArgument, "an id name must not be empty");
  }
  return debugRegisterNameAs(domain, hashId(domain, name), name);
}

Result<uint64_t> Runtime::debugRegisterNameAs(IdDomain domain, uint64_t id, std::string_view name)
{
  if (id == 0) {
    return Status::error(StatusCode::InvalidArgument, "name hashes to the null id");
  }
  string owned(std::string(name).c_str());
  auto &names = names_[int(domain)];
  if (const string *existing = names.lookup_ptr(id)) {
    if (*existing == owned) {
      return id;
    }
    char msg[512];
    snprintf(msg,
             sizeof(msg),
             "id collision in domain '%.*s': '%s' and '%s' hash to %016llx",
             int(idDomainTag(domain).size()),
             idDomainTag(domain).data(),
             existing->c_str(),
             owned.c_str(),
             (unsigned long long)id);
    return Status::error(StatusCode::AlreadyExists, msg);
  }
  names.add(id, owned);
  return id;
}

const char *Runtime::nameOf(IdDomain domain, uint64_t id) const
{
  const string *s = names_[int(domain)].lookup_ptr(id);
  return s ? s->c_str() : nullptr;
}

} // namespace coredcc

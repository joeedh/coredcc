#include "coredcc/lib/attachment.h"

#include <string>
#include <utility>

namespace coredcc {

AttachmentTypeRegistry::~AttachmentTypeRegistry()
{
  for (Entry *e : entries_) {
    litestl::alloc::Delete(e);
  }
}

Result<TypeId> AttachmentTypeRegistry::add(AttachmentTypeInfo info,
                                           Owned<AttachmentProvider> provider)
{
  const std::string what = "attachment type '" + std::string(info.name.c_str()) + "'";
  if (!provider) {
    return Status::error(StatusCode::InvalidArgument,
                         (what + " has no provider").c_str());
  }
  if (provider->hasSerialize() != provider->hasDeserialize()) {
    return Status::error(
        StatusCode::InvalidArgument,
        (what + " has one of serialize and deserialize: it needs both or neither")
            .c_str());
  }
  const bool canSave = provider->hasSerialize();
  if (info.storage == (canSave ? BlockStorage::Transient : BlockStorage::Payload)) {
    return Status::error(StatusCode::InvalidArgument,
                         (what + (canSave ? " is Transient, but has serialize"
                                          : " is Payload, but has no serialize"))
                             .c_str());
  }
  if (canSave) {
    info.storage = BlockStorage::Payload;
  }
  if (info.onDuplicate == DuplicatePolicy::Copy && !provider->hasCopy()) {
    return Status::error(StatusCode::InvalidArgument,
                         (what + " is copied with its block, but has no copy").c_str());
  }
  Result<TypeId> id = rt_.registerId<IdDomain::Type>(info.name.c_str());
  if (!id.ok()) {
    return id.status();
  }
  if (byId_.lookup_ptr(id->value)) {
    return Status::error(StatusCode::AlreadyExists,
                         (what + " is already registered").c_str());
  }
  const BlockTypeRegistry *blocks = rt_.findExt<BlockTypeRegistry>();
  if (blocks && blocks->find(*id)) {
    return Status::error(StatusCode::AlreadyExists,
                         (what + ": a block type has this name").c_str());
  }
  Entry *e = litestl::alloc::New<Entry>("coredcc::AttachmentTypeRegistry::Entry");
  e->info = std::move(info);
  e->id = *id;
  e->provider = std::move(provider);
  entries_.append(e);
  byId_.add(id->value, e);
  return *id;
}

const AttachmentTypeRegistry::Entry *AttachmentTypeRegistry::find(TypeId type) const
{
  Entry *const *e = byId_.lookup_ptr(type.value);
  return e ? *e : nullptr;
}

Status AttachmentTypeRegistry::validate() const
{
  if (int(byId_.size()) != int(entries_.size())) {
    return Status::error(StatusCode::Internal,
                         "AttachmentTypeRegistry: index and list differ");
  }
  for (const Entry *e : entries_) {
    if (find(e->id) != e || !e->provider || !rt_.nameOf(e->id)) {
      return Status::error(StatusCode::Internal, "AttachmentTypeRegistry: a bad entry");
    }
  }
  return Status();
}

} // namespace coredcc

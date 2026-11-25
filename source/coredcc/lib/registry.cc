#include "coredcc/lib/registry.h"

#include "coredcc/lib/attachment.h"

#include <string>

namespace coredcc {

ClientRegistry::ClientRegistry(Runtime *rt) : rt_(*rt)
{
  CDCC_CHECK(registerClient(kCoredccClient).ok(), "registering the coredcc client");
}

Result<ClientId> ClientRegistry::registerClient(std::string_view name,
                                                std::span<const char *const> aliases)
{
  Result<ClientId> id = rt_.registerId<IdDomain::Client>(name);
  if (!id.ok()) {
    return id.status();
  }
  if (aliases_.lookup_ptr(id->value)) {
    return Status::error(
        StatusCode::AlreadyExists,
        ("client '" + std::string(name) + "' is already an alias").c_str());
  }
  /* Check every alias before adding any, so a failure changes nothing. */
  for (const char *alias : aliases) {
    const ClientId a = ClientId::of(alias);
    const uint64_t *target = aliases_.lookup_ptr(a.value);
    if ((target && *target != id->value) || clients_.contains(a)) {
      return Status::error(
          StatusCode::AlreadyExists,
          ("alias '" + std::string(alias) + "' is already in use").c_str());
    }
    if (a == *id) {
      return Status::error(
          StatusCode::InvalidArgument,
          ("client '" + std::string(name) + "' can't alias itself").c_str());
    }
  }
  for (const char *alias : aliases) {
    Result<ClientId> a = rt_.registerId<IdDomain::Client>(alias);
    if (!a.ok()) {
      return a.status();
    }
    aliases_.add_overwrite(a->value, id->value);
    generation_++;
  }
  if (!clients_.contains(*id)) {
    clients_.append(*id);
  }
  return *id;
}

bool ClientRegistry::isRegistered(ClientId id) const
{
  return clients_.contains(canonical(id));
}

ClientId ClientRegistry::canonical(ClientId id) const
{
  const uint64_t *target = aliases_.lookup_ptr(id.value);
  return target ? ClientId{*target} : id;
}

Status ClientRegistry::validate() const
{
  for (const ClientId &c : clients_) {
    if (!rt_.nameOf(c) || aliases_.lookup_ptr(c.value)) {
      return Status::error(StatusCode::Internal,
                           "ClientRegistry: a client is unnamed or also an alias");
    }
  }
  return Status();
}

BlockTypeRegistry::~BlockTypeRegistry()
{
  for (Entry *e : entries_) {
    litestl::alloc::Delete(e);
  }
}

Result<TypeId> BlockTypeRegistry::add(BlockTypeInfo info, Owned<BlockProvider> provider)
{
  const std::string name = info.name.c_str();
  if (!provider) {
    return Status::error(StatusCode::InvalidArgument,
                         ("block type '" + name + "' has no provider").c_str());
  }
  const bool canSave = provider->hasSerialize();
  if (canSave != provider->hasDeserialize()) {
    return Status::error(
        StatusCode::InvalidArgument,
        ("block type '" + name +
         "' has one of serialize and deserialize: it needs both or neither")
            .c_str());
  }
  if (info.storage == (canSave ? BlockStorage::Transient : BlockStorage::Payload)) {
    return Status::error(StatusCode::InvalidArgument,
                         ("block type '" + name +
                          (canSave ? "' is Transient, but has serialize"
                                   : "' is Payload, but has no serialize"))
                             .c_str());
  }
  if (canSave) {
    info.storage = BlockStorage::Payload;
  }
  ClientRegistry &clients = rt_.ext<ClientRegistry>();
  if (!clients.isRegistered(info.client)) {
    return Status::error(
        StatusCode::NotFound,
        ("block type '" + name + "': its client is not registered").c_str());
  }
  info.client = clients.canonical(info.client);
  Result<TypeId> id = rt_.registerId<IdDomain::Type>(name);
  if (!id.ok()) {
    return id.status();
  }
  if (byId_.lookup_ptr(id->value)) {
    return Status::error(StatusCode::AlreadyExists,
                         ("block type '" + name + "' is already registered").c_str());
  }
  const AttachmentTypeRegistry *attachments = rt_.findExt<AttachmentTypeRegistry>();
  if (attachments && attachments->find(*id)) {
    return Status::error(
        StatusCode::AlreadyExists,
        ("block type '" + name + "': an attachment type has this name").c_str());
  }
  Entry *e = litestl::alloc::New<Entry>("coredcc::BlockTypeRegistry::Entry");
  e->info = std::move(info);
  e->id = *id;
  e->provider = std::move(provider);
  entries_.append(e);
  byId_.add(id->value, e);
  return *id;
}

const BlockTypeRegistry::Entry *BlockTypeRegistry::find(TypeId type) const
{
  Entry *const *e = byId_.lookup_ptr(type.value);
  return e ? *e : nullptr;
}

BlockProvider BlockTypeRegistry::provider(TypeId type) const
{
  const Entry *e = find(type);
  return e ? e->provider.get() : BlockProvider();
}

Status BlockTypeRegistry::validate() const
{
  if (int(byId_.size()) != int(entries_.size())) {
    return Status::error(StatusCode::Internal,
                         "BlockTypeRegistry: index and list differ");
  }
  for (const Entry *e : entries_) {
    if (find(e->id) != e || !e->provider || !rt_.nameOf(e->id)) {
      return Status::error(StatusCode::Internal, "BlockTypeRegistry: a bad entry");
    }
  }
  return Status();
}

} // namespace coredcc

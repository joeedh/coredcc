/* Placeholders: adding, filling and cutting them, and renaming any record. */
#include "coredcc/lib/library.h"

#include <string>
#include <utility>

namespace coredcc {

using litestl::util::Vector;

namespace {

Status noBlock(const char *op)
{
  return Status::error(StatusCode::NotFound, (std::string(op) + ": no such block").c_str());
}

} // namespace

Result<BlockHandle> Library::addPlaceholder(BlockId id, PlaceholderInfo info)
{
  CDCC_TRY(busyIfSweeping("addPlaceholder"));
  id = clients_.canonical(id);
  if (id.local.isNull() || find(id)) {
    return Status::error(StatusCode::AlreadyExists,
                         "addPlaceholder: the id is null or already has a record");
  }
  const bool noType = info.typeName.size() == 0;
  if (noType && info.reason == PlaceholderReason::MissingType) {
    return Status::error(StatusCode::InvalidArgument,
                         "addPlaceholder: a MissingType placeholder needs its type's name");
  }
  const TypeId type = noType ? TypeId() : TypeId::of(info.typeName.c_str());
  Result<BlockHandle> h = addRecord(type, id, info.name.c_str(), nullptr);
  Datablock *b = rec(*h);
  b->state_ = BlockState::Placeholder;
  b->placeholderReason_ = info.reason;
  b->placeholderType_ = std::move(info.typeName);
  b->sourceName_ = std::move(info.sourceName);
  b->placeholderData_ = std::move(info.payload);
  return h;
}

Result<BlockHandle> Library::addPlaceholder(BlockId id,
                                            std::string_view typeName,
                                            std::string_view name,
                                            Vector<uint8_t> payload)
{
  PlaceholderInfo info;
  info.typeName = std::string(typeName).c_str();
  info.name = std::string(name).c_str();
  info.payload.bytes = std::move(payload);
  return addPlaceholder(id, std::move(info));
}

Result<BlockHandle> Library::adopt(BlockHandle h,
                                   const BlockTypeRegistry::Entry &type,
                                   std::string_view name,
                                   void *clientData)
{
  Datablock *b = rec(h);
  if (b->type_ && b->type_ != type.id) {
    return Status::error(StatusCode::TypeMismatch,
                         ("createWithId: the block was saved as a '" +
                          std::string(b->placeholderType_.c_str()) + "', not a '" +
                          type.info.name.c_str() + "'")
                             .c_str());
  }
  b->type_ = type.id;
  b->name_ = std::string(name).c_str();
  b->clientData_ = clientData;
  b->placeholderType_ = litestl::util::string();
  b->placeholderData_ = BlockPayload(); /* the provider reports the references now */
  setState(*b, BlockState::Live);
  return h;
}

Status Library::relinkRef(BlockHandle placeholder, int index, BlockId target)
{
  CDCC_TRY(busyIfSweeping("relinkRef"));
  Datablock *b = rec(placeholder);
  if (!b) {
    return noBlock("relinkRef");
  }
  if (b->state_ != BlockState::Placeholder) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("relinkRef: block '" + std::string(b->name_.c_str()) + "' is " +
                          blockStateName(b->state_) + ", not a placeholder")
                             .c_str());
  }
  if (index < 0 || index >= int(b->placeholderData_.refs.size())) {
    return Status::error(StatusCode::OutOfRange, "relinkRef: no such row in the ref table");
  }
  RefCell &row = b->placeholderData_.refs[index];
  row.target = target.isNull() ? target : clients_.canonical(target);
  row.cache = BlockHandle();
  epoch_++;
  return Status();
}

Status Library::rename(BlockHandle h, std::string_view name)
{
  CDCC_TRY(busyIfSweeping("rename"));
  Datablock *b = rec(h);
  if (!b) {
    return noBlock("rename");
  }
  b->name_ = std::string(name).c_str();
  epoch_++;
  return Status();
}

} // namespace coredcc

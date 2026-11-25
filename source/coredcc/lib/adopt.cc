/* MissingBlock records that are filled in place, and taking that back (architecture §8a). */
#include "coredcc/lib/io/format.h"
#include "coredcc/lib/library.h"
#include "coredcc/lib/type_names.h"

#include <string>
#include <utility>

namespace coredcc {

using litestl::util::string;
using litestl::util::Vector;

namespace {

Status refused(const char *op, const Datablock &b, const char *why)
{
  return Status::error(StatusCode::FailedPrecondition,
                       (std::string(op) + ": block '" + b.name().c_str() + "' " + why).c_str());
}

} // namespace

bool Library::adopted(BlockHandle h) const
{
  const Datablock *b = rec(h);
  return b && b->adoption_ && b->adoption_->was.size() > 0;
}

void Library::keepMissing(Datablock &b)
{
  if (!b.adoption_) {
    b.adoption_ = std::make_unique<Datablock::Adoption>();
  }
  Datablock::Missing a;
  a.type = b.type_;
  a.name = b.name_;
  a.typeName = std::exchange(b.placeholderType_, string());
  a.sourceName = std::exchange(b.sourceName_, string());
  a.sourceStored = std::exchange(b.sourceStored_, false);
  a.data = std::exchange(b.placeholderData_, BlockPayload());
  a.flags = b.flags_;
  a.hadExtension = b.fileExtension_ != nullptr;
  a.slots = b.slots_;
  for (const Datablock::Attachment &x : b.attachments_) {
    a.attachments.append(x.type);
  }
  if (b.opaque_) {
    for (const OpaqueAttachment &o : *b.opaque_) {
      a.attachments.append(detail::typeNamed(o.typeName.c_str()));
    }
  }
  b.adoption_->was.append(std::move(a));
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
  if (detail::libio::hasCriticalField(b->fileExtension())) {
    return Status::error(StatusCode::Unsupported,
                         "createWithId: the block's record in the file needs a newer build");
  }
  keepMissing(*b);
  b->type_ = type.id;
  b->name_ = std::string(name).c_str();
  b->clientData_ = clientData;
  setState(*b, BlockState::Live); /* the provider reports the references now */
  return h;
}

/* What came with the data goes with it: `from` keeps what it had while it was missing. */
void Library::splitAdopted(Datablock &from, Datablock &to)
{
  const Datablock::Missing &a = from.adoption_->was.last();
  Vector<Datablock::Attachment> stay;
  for (const Datablock::Attachment &x : from.attachments_) {
    (a.attachments.contains(x.type) ? stay : to.attachments_).append(x);
  }
  from.attachments_ = std::move(stay);
  if (from.opaque_) {
    const auto all = std::move(from.opaque_);
    for (OpaqueAttachment &o : *all) {
      const bool had = a.attachments.contains(detail::typeNamed(o.typeName.c_str()));
      (had ? from : to).addOpaque(std::move(o));
    }
  }
  if (!a.hadExtension) {
    to.fileExtension_ = std::move(from.fileExtension_);
  }
}

Result<BlockHandle> Library::unadopt(BlockHandle h)
{
  CDCC_TRY(busyIfSweeping("unadopt"));
  Datablock *b = rec(h);
  if (!b) {
    return Status::error(StatusCode::NotFound, "unadopt: no such block");
  }
  if (!adopted(h)) {
    return refused("unadopt", *b, "didn't fill a missing block");
  }
  if (b->state_ == BlockState::InHistory || b->state_ == BlockState::Doomed) {
    return refused("unadopt", *b, "is deleted");
  }
  const BlockHandle kept = pool_.create();
  Datablock *k = rec(kept);
  k->lib_ = this;
  k->id_ = b->id_;
  k->state_ = BlockState::Doomed;
  k->deletedFrom_ = b->state_;
  doomedCount_++;
  splitAdopted(*b, *k);
  std::swap(k->overrides_, b->overrides_);
  Datablock::Missing a = b->adoption_->was.pop_back();
  k->flags_ = std::exchange(b->flags_, a.flags);
  k->type_ = std::exchange(b->type_, a.type);
  k->clientData_ = std::exchange(b->clientData_, nullptr);
  k->name_ = std::exchange(b->name_, a.name);
  k->slots_ = std::exchange(b->slots_, std::move(a.slots));
  k->placeholderReason_ = std::exchange(b->placeholderReason_, PlaceholderReason::MissingBlock);
  k->placeholderType_ = std::exchange(b->placeholderType_, a.typeName);
  k->placeholderData_ = std::exchange(b->placeholderData_, std::move(a.data));
  k->sourceName_ = std::exchange(b->sourceName_, a.sourceName);
  k->sourceStored_ = std::exchange(b->sourceStored_, a.sourceStored);
  b->adoption_->kept = kept;
  if (b->pins(PinKind::Host) > 0) {
    k->pins_[int(PinKind::Host)]++; /* whoever pinned the block holds its data */
    b->adoption_->lent.append(kept);
  }
  setState(*b, BlockState::Placeholder);
  return kept;
}

Status Library::readopt(BlockHandle h, BlockHandle kept)
{
  CDCC_TRY(busyIfSweeping("readopt"));
  Datablock *b = rec(h);
  Datablock *k = rec(kept);
  if (!b || !k || k->state_ != BlockState::Doomed ||
      clients_.canonical(k->id_) != clients_.canonical(b->id_))
  {
    return Status::error(StatusCode::NotFound, "readopt: no such block, or its data is gone");
  }
  if (b->state_ != BlockState::Placeholder ||
      b->placeholderReason_ != PlaceholderReason::MissingBlock)
  {
    return refused("readopt", *b, "isn't missing any more");
  }
  for (const Datablock::Attachment &x : k->attachments_) {
    if (hasAttachment(h, x.type)) {
      return refused("readopt", *b, "got an attachment that its data has too");
    }
  }
  keepMissing(*b);
  for (Datablock::Attachment &x : k->attachments_) {
    b->attachments_.append(x);
  }
  if (k->opaque_) {
    for (OpaqueAttachment &o : *k->opaque_) {
      b->addOpaque(std::move(o));
    }
  }
  if (k->fileExtension_) {
    b->fileExtension_ = std::move(k->fileExtension_);
  }
  b->overrides_ = std::move(k->overrides_);
  b->flags_ = k->flags_;
  b->type_ = k->type_;
  b->clientData_ = k->clientData_;
  b->name_ = k->name_;
  b->slots_ = std::move(k->slots_);
  b->placeholderReason_ = k->placeholderReason_;
  b->placeholderType_ = k->placeholderType_;
  b->placeholderData_ = std::move(k->placeholderData_);
  b->sourceName_ = k->sourceName_;
  b->sourceStored_ = k->sourceStored_;
  const BlockState state = k->deletedFrom_;
  b->adoption_->kept = BlockHandle();
  b->adoption_->dropLent(kept);
  pool_.destroy(kept); /* the record alone: its data is the block's again */
  doomedCount_--;
  setState(*b, state);
  return Status();
}

void Library::releaseLent(Datablock &b)
{
  if (!b.adoption_) {
    return;
  }
  const Vector<BlockHandle> lent = std::exchange(b.adoption_->lent, Vector<BlockHandle>());
  for (BlockHandle h : lent) {
    unpin(h, PinKind::Host);
  }
}

} // namespace coredcc

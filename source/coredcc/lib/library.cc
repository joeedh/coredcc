#include "coredcc/lib/library.h"

#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::string;
using litestl::util::Vector;

const char *blockStateName(BlockState state)
{
  switch (state) {
    case BlockState::Live:
      return "Live";
    case BlockState::InHistory:
      return "InHistory";
    case BlockState::Detached:
      return "Detached";
    case BlockState::Placeholder:
      return "Placeholder";
  }
  return "?";
}

namespace {

string toLs(std::string_view s)
{
  return string(std::string(s).c_str());
}

Status wrongState(const Datablock &b, const char *op, BlockState want)
{
  return Status::error(StatusCode::FailedPrecondition,
                       (std::string(op) + ": block '" + b.name().c_str() + "' is " +
                        blockStateName(b.state()) + ", not " + blockStateName(want))
                           .c_str());
}

Status noBlock(const char *op)
{
  return Status::error(StatusCode::NotFound, (std::string(op) + ": no such block").c_str());
}

} // namespace

Library::Library(Runtime *rt)
    : rt_(*rt), types_(rt->ext<BlockTypeRegistry>()), clients_(rt->ext<ClientRegistry>())
{
#ifdef NDEBUG
  audit_ = false;
#else
  audit_ = true;
#endif
}

Library::~Library()
{
  Vector<BlockHandle> all;
  pool_.forEach([&all](BlockHandle h, Datablock &) { all.append(h); });
  destroyBlocks(all);
}

Status Library::busyIfSweeping(const char *what) const
{
  if (!sweeping_ && !collecting_) {
    return Status();
  }
  return Status::error(StatusCode::Busy,
                       (std::string(what) + ": the library is sweeping (called from a "
                                            "provider's destroy)")
                           .c_str());
}

void Library::runOrDefer(litestl::util::function<void()> fn)
{
  if (sweeping_) {
    deferred_.append(std::move(fn));
  }
  else {
    fn();
  }
}

/* ---- creating ---- */

Result<BlockHandle> Library::addRecord(TypeId type,
                                       BlockId id,
                                       std::string_view name,
                                       void *clientData)
{
  const BlockHandle h = pool_.create();
  Datablock *b = rec(h);
  b->id_ = id;
  b->type_ = type;
  b->name_ = toLs(name);
  b->clientData_ = clientData;
  byId_.add(id, h);
  epoch_++;
  return h;
}

Result<BlockHandle> Library::create(TypeId type, std::string_view name, void *clientData)
{
  CDCC_TRY(busyIfSweeping("create"));
  const BlockTypeRegistry::Entry *entry = types_.find(type);
  if (!entry) {
    return Status::error(StatusCode::NotFound, "create: the block type is not registered");
  }
  /* A repeated id from the GUID source would corrupt the index; retry, then fail. */
  for (int attempt = 0; attempt < 4; attempt++) {
    const BlockId id{entry->info.client, rt_.newLocalId()};
    if (!id.local.isNull() && !find(id)) {
      return addRecord(type, id, name, clientData);
    }
  }
  return Status::error(StatusCode::Internal,
                       "create: the GUID source keeps returning ids that are in use");
}

Result<BlockHandle> Library::createWithId(TypeId type,
                                          BlockId id,
                                          std::string_view name,
                                          void *clientData)
{
  CDCC_TRY(busyIfSweeping("createWithId"));
  const BlockTypeRegistry::Entry *entry = types_.find(type);
  if (!entry) {
    return Status::error(StatusCode::NotFound,
                         "createWithId: the block type is not registered");
  }
  id = clients_.canonical(id);
  if (id.local.isNull()) {
    return Status::error(StatusCode::InvalidArgument, "createWithId: the LocalId is null");
  }
  if (id.client != entry->info.client) {
    return Status::error(StatusCode::InvalidArgument,
                         "createWithId: the id's client is not the block type's client");
  }
  if (find(id)) {
    return Status::error(StatusCode::AlreadyExists,
                         ("createWithId: a block with this id exists ('" +
                          std::string(rec(find(id))->name_.c_str()) + "')")
                             .c_str());
  }
  return addRecord(type, id, name, clientData);
}

Result<BlockHandle> Library::reviveAs(TypeId type,
                                      BlockId id,
                                      std::string_view name,
                                      void *clientData)
{
  CDCC_TRY(busyIfSweeping("reviveAs"));
  if (const BlockHandle h = find(id)) {
    const Datablock &b = *rec(h);
    if (b.state_ == BlockState::Live) {
      return Status::error(StatusCode::AlreadyExists,
                           ("reviveAs: block '" + std::string(b.name_.c_str()) +
                            "' is already Live")
                               .c_str());
    }
    return Status::error(StatusCode::FailedPrecondition,
                         ("reviveAs: block '" + std::string(b.name_.c_str()) + "' is " +
                          blockStateName(b.state_) + "; restore it instead")
                             .c_str());
  }
  return createWithId(type, id, name, clientData);
}

Result<BlockHandle> Library::addPlaceholder(BlockId id,
                                            std::string_view typeName,
                                            std::string_view name,
                                            Vector<uint8_t> payload)
{
  CDCC_TRY(busyIfSweeping("addPlaceholder"));
  id = clients_.canonical(id);
  if (id.local.isNull() || find(id)) {
    return Status::error(StatusCode::AlreadyExists,
                         "addPlaceholder: the id is null or already has a record");
  }
  Result<BlockHandle> h = addRecord(TypeId::of(typeName), id, name, nullptr);
  Datablock *b = rec(*h);
  b->state_ = BlockState::Placeholder;
  b->placeholderType_ = toLs(typeName);
  b->placeholderPayload_ = std::move(payload);
  return h;
}

/* ---- finding ---- */

void Library::renormalizeIds() const
{
  if (clientGeneration_ == clients_.generation()) {
    return;
  }
  clientGeneration_ = clients_.generation();
  Library *self = const_cast<Library *>(this);
  Vector<BlockHandle> stale;
  forEach([&](BlockHandle h, const Datablock &b) {
    if (!(clients_.canonical(b.id_) == b.id_)) {
      stale.append(h);
    }
  });
  for (BlockHandle h : stale) {
    Datablock *b = self->rec(h);
    const BlockId c = clients_.canonical(b->id_);
    if (byId_.lookup_ptr(c)) {
      /* Two records for one block: keep both findable by their old ids and say so. */
      rt_.setStickyError(Status::error(
          StatusCode::AlreadyExists,
          ("Library: block '" + std::string(b->name_.c_str()) +
           "' was saved under a client that is now an alias, and its canonical id has "
           "another record")
              .c_str()));
      continue;
    }
    self->byId_.remove(b->id_);
    b->id_ = c;
    self->byId_.add(c, h);
  }
  if (stale.size() > 0) {
    self->epoch_++;
  }
}

BlockHandle Library::find(BlockId id) const
{
  renormalizeIds();
  const BlockHandle *h = byId_.lookup_ptr(clients_.canonical(id));
  return h ? *h : BlockHandle();
}

const Datablock *Library::get(BlockHandle h) const
{
  return rec(h);
}

/* ---- states ---- */

Status Library::markDeleted(BlockHandle h)
{
  CDCC_TRY(busyIfSweeping("markDeleted"));
  Datablock *b = rec(h);
  if (!b) {
    return noBlock("markDeleted");
  }
  if (b->state_ != BlockState::Live) {
    return wrongState(*b, "markDeleted", BlockState::Live);
  }
  b->state_ = BlockState::InHistory;
  epoch_++;
  return Status();
}

Status Library::restoreFromHistory(BlockHandle h)
{
  CDCC_TRY(busyIfSweeping("restoreFromHistory"));
  Datablock *b = rec(h);
  if (!b) {
    return noBlock("restoreFromHistory");
  }
  if (b->state_ != BlockState::InHistory) {
    return wrongState(*b, "restoreFromHistory", BlockState::InHistory);
  }
  b->state_ = BlockState::Live;
  epoch_++;
  return Status();
}

Status Library::destroyFromHistory(BlockHandle h)
{
  CDCC_TRY(busyIfSweeping("destroyFromHistory"));
  Datablock *b = rec(h);
  if (!b) {
    return noBlock("destroyFromHistory");
  }
  if (b->state_ != BlockState::InHistory) {
    return wrongState(*b, "destroyFromHistory", BlockState::InHistory);
  }
  /* A Live or Detached block's Strong ref would dangle: the delete should have taken
   * that user too. Refuse; the block stays InHistory, which GC treats as a root. */
  const BlockId id = b->id_;
  for (BlockHandle user : usersOf(h)) {
    const Datablock *u = rec(user);
    if (u->state_ != BlockState::Live && u->state_ != BlockState::Detached) {
      continue;
    }
    bool strong = false;
    foreachRef(user, [&](RefCell &cell) {
      strong |= cell.kind == RefKind::Strong && clients_.canonical(cell.target) == id;
    }).ignore();
    if (strong) {
      return Status::error(StatusCode::FailedPrecondition,
                           ("destroyFromHistory: '" + std::string(u->name_.c_str()) +
                            "' still holds a Strong ref to '" + b->name_.c_str() + "'")
                               .c_str());
    }
  }
  Vector<BlockHandle> one;
  one.append(h);
  destroyBlocks(one);
  if (rt_.hasStickyError()) {
    return rt_.takeStickyError(); /* the provider's destroy failed; the block is gone */
  }
  return Status();
}

Status Library::onHostDestroyed(BlockId id)
{
  CDCC_TRY(busyIfSweeping("onHostDestroyed"));
  Datablock *b = rec(find(id));
  if (!b) {
    return noBlock("onHostDestroyed");
  }
  if (b->state_ != BlockState::Live) {
    return wrongState(*b, "onHostDestroyed", BlockState::Live);
  }
  b->state_ = BlockState::Detached;
  epoch_++;
  return Status();
}

Status Library::onHostRestored(BlockId id)
{
  CDCC_TRY(busyIfSweeping("onHostRestored"));
  Datablock *b = rec(find(id));
  if (!b) {
    return noBlock("onHostRestored");
  }
  if (b->state_ != BlockState::Detached) {
    return wrongState(*b, "onHostRestored", BlockState::Detached);
  }
  b->state_ = BlockState::Live;
  epoch_++;
  return Status();
}

/* ---- flags and pins ---- */

void Library::setFlag(BlockHandle h, BlockFlags flag, bool on)
{
  CDCC_CHECK(flag != BlockFlags::HasOverrides, "HasOverrides follows setOverride");
  runOrDefer([this, h, flag, on]() {
    if (Datablock *b = rec(h)) {
      b->flags_ = on ? (b->flags_ | flag) : (b->flags_ & ~flag);
    }
  });
}

void Library::pin(BlockHandle h)
{
  runOrDefer([this, h]() {
    if (Datablock *b = rec(h)) {
      b->pins_++;
    }
  });
}

void Library::unpin(BlockHandle h)
{
  runOrDefer([this, h]() {
    if (Datablock *b = rec(h)) {
      CDCC_CHECK(b->pins_ > 0, "unpin without a pin");
      b->pins_--;
    }
  });
}

/* ---- data and references ---- */

void *Library::data(BlockHandle h) const
{
  const Datablock *b = rec(h);
  if (!b || b->state_ != BlockState::Live) {
    return nullptr;
  }
  BlockProvider p = types_.provider(b->type_);
  return p ? p.data(*b) : nullptr;
}

BlockHandle Library::resolveRef(const RefCell &cell) const
{
  if (cell.target.isNull()) {
    return BlockHandle();
  }
  const BlockId want = clients_.canonical(cell.target);
  const Datablock *cached = rec(cell.cache);
  if (!cached || cached->id_ != want) {
    cell.cache = find(want);
    cached = rec(cell.cache);
  }
  return cached && cached->state_ == BlockState::Live ? cell.cache : BlockHandle();
}

Status Library::writeRef(RefCell &cell, BlockId target)
{
  CDCC_TRY(busyIfSweeping("writeRef"));
  if (cell.kind == RefKind::Strong && target.isNull()) {
    return Status::error(StatusCode::InvalidArgument, "writeRef: a Strong ref can't be null");
  }
  cell.target = target.isNull() ? target : clients_.canonical(target);
  cell.cache = find(cell.target);
  epoch_++;
  return Status();
}

void Library::noteRefsChanged()
{
  runOrDefer([this]() { epoch_++; });
}

/* ---- slots ---- */

Status Library::setSlot(BlockHandle owner, TypeId ns, int64_t key, BlockId target, RefKind kind)
{
  CDCC_TRY(busyIfSweeping("setSlot"));
  Datablock *b = rec(owner);
  if (!b) {
    return noBlock("setSlot");
  }
  if (kind == RefKind::Strong && target.isNull()) {
    return Status::error(StatusCode::InvalidArgument, "setSlot: a Strong ref can't be null");
  }
  Datablock::Slot *slot = nullptr;
  for (Datablock::Slot &s : b->slots_) {
    if (s.ns == ns && s.key == key) {
      slot = &s;
    }
  }
  if (!slot) {
    b->slots_.append(Datablock::Slot{ns, key, RefCell(kind)});
    slot = &b->slots_.last();
  }
  slot->cell.kind = kind;
  slot->cell.target = target.isNull() ? target : clients_.canonical(target);
  slot->cell.cache = find(slot->cell.target);
  epoch_++;
  return Status();
}

const RefCell *Library::slot(BlockHandle owner, TypeId ns, int64_t key) const
{
  const Datablock *b = rec(owner);
  if (!b) {
    return nullptr;
  }
  for (const Datablock::Slot &s : b->slots_) {
    if (s.ns == ns && s.key == key) {
      return &s.cell;
    }
  }
  return nullptr;
}

Status Library::forceUnlink(BlockHandle owner, TypeId ns, int64_t key)
{
  CDCC_TRY(busyIfSweeping("forceUnlink"));
  Datablock *b = rec(owner);
  if (!b) {
    return noBlock("forceUnlink");
  }
  for (int i = 0; i < int(b->slots_.size()); i++) {
    if (b->slots_[i].ns == ns && b->slots_[i].key == key) {
      b->slots_.remove_at(i);
      epoch_++;
      return Status();
    }
  }
  return Status::error(StatusCode::NotFound, "forceUnlink: no such slot");
}

Status Library::foreachRef(BlockHandle h, litestl::util::function<void(RefCell &)> fn) const
{
  Datablock *b = const_cast<Library *>(this)->rec(h);
  if (!b) {
    return noBlock("foreachRef");
  }
  /* The provider can enumerate while its data or wrapper exists: Live, held by the
   * history, or Detached (whose targets must survive until the host restores it). */
  if (b->state_ == BlockState::Live || b->state_ == BlockState::InHistory ||
      b->state_ == BlockState::Detached)
  {
    if (BlockProvider p = types_.provider(b->type_)) {
      const bool stickyBefore = rt_.hasStickyError();
      RefVisitor visitor([](void *ctx, RefCell &cell) {
        (*static_cast<litestl::util::function<void(RefCell &)> *>(ctx))(cell);
      }, &fn);
      p.foreachRef(*b, visitor);
      if (!stickyBefore && rt_.hasStickyError()) {
        return Status::error(StatusCode::Internal,
                             ("foreachRef: the provider of '" + std::string(b->name_.c_str()) +
                              "' failed; see the Runtime's sticky error")
                                 .c_str());
      }
    }
  }
  for (Datablock::Slot &s : b->slots_) {
    fn(s.cell);
  }
  return Status();
}

/* ---- interfaces ---- */

IfaceRef Library::queryRaw(BlockHandle h, InterfaceId iface) const
{
  const Datablock *b = rec(h);
  if (!b || b->state_ != BlockState::Live) {
    return IfaceRef();
  }
  if (b->hasFlag(BlockFlags::HasOverrides)) {
    for (const Datablock::Override &o : b->overrides_) {
      if (o.iface == iface) {
        return o.ref;
      }
    }
  }
  if (void *self = data(h)) {
    if (const IfaceTable *t = rt_.ifaces().find(b->type_, iface)) {
      return IfaceRef(t, self);
    }
  }
  BlockProvider p = types_.provider(b->type_);
  if (p && p.hasQueryInterface()) {
    const QueryAnswer a = p.queryInterface(*b, iface);
    if (a.implType) {
      if (const IfaceTable *t = rt_.ifaces().find(a.implType, iface)) {
        return IfaceRef(t, a.self);
      }
    }
  }
  return IfaceRef();
}

Status Library::setOverride(BlockHandle h, InterfaceId iface, IfaceRef ref)
{
  CDCC_TRY(busyIfSweeping("setOverride"));
  Datablock *b = rec(h);
  if (!b) {
    return noBlock("setOverride");
  }
  if (!ref || ref.table()->iface != iface) {
    return Status::error(StatusCode::TypeMismatch,
                         "setOverride: the interface value is null or of another interface");
  }
  for (Datablock::Override &o : b->overrides_) {
    if (o.iface == iface) {
      o.ref = ref;
      return Status();
    }
  }
  b->overrides_.append(Datablock::Override{iface, ref});
  b->flags_ = b->flags_ | BlockFlags::HasOverrides;
  return Status();
}

void Library::clearOverride(BlockHandle h, InterfaceId iface)
{
  runOrDefer([this, h, iface]() {
    Datablock *b = rec(h);
    if (!b) {
      return;
    }
    for (int i = 0; i < int(b->overrides_.size()); i++) {
      if (b->overrides_[i].iface == iface) {
        b->overrides_.remove_at(i);
        break;
      }
    }
    if (b->overrides_.size() == 0) {
      b->flags_ = b->flags_ & ~BlockFlags::HasOverrides;
    }
  });
}

/* ---- attachments ---- */

Status Library::attach(BlockHandle h, TypeId type, void *data, void (*destroy)(void *data))
{
  CDCC_TRY(busyIfSweeping("attach"));
  Datablock *b = rec(h);
  if (!b) {
    return noBlock("attach");
  }
  if (attachment(h, type)) {
    return Status::error(StatusCode::AlreadyExists, "attach: the block has this attachment");
  }
  b->attachments_.append(Datablock::Attachment{type, data, destroy});
  return Status();
}

Result<void *> Library::detach(BlockHandle h, TypeId type)
{
  CDCC_TRY(busyIfSweeping("detach"));
  Datablock *b = rec(h);
  if (!b) {
    return noBlock("detach");
  }
  for (int i = 0; i < int(b->attachments_.size()); i++) {
    if (b->attachments_[i].type == type) {
      void *data = b->attachments_[i].data;
      b->attachments_.remove_at(i);
      return data;
    }
  }
  return Status::error(StatusCode::NotFound, "detach: no such attachment");
}

void *Library::attachment(BlockHandle h, TypeId type) const
{
  const Datablock *b = rec(h);
  if (!b) {
    return nullptr;
  }
  for (const Datablock::Attachment &a : b->attachments_) {
    if (a.type == type) {
      return a.data;
    }
  }
  return nullptr;
}

/* ---- validation ---- */

Status Library::validate() const
{
  if (int(byId_.size()) != pool_.size()) {
    return Status::error(StatusCode::Internal,
                         "Library: the id index and the records differ in size");
  }
  renormalizeIds();
  Status status;
  forEach([&](BlockHandle h, const Datablock &b) {
    const BlockHandle *indexed = byId_.lookup_ptr(b.id_);
    if (status.ok() && (!indexed || *indexed != h)) {
      status = Status::error(StatusCode::Internal,
                             ("Library: block '" + std::string(b.name_.c_str()) +
                              "' is not indexed under its id, or shares it")
                                 .c_str());
    }
    if (status.ok() && b.hasFlag(BlockFlags::HasOverrides) != (b.overrides_.size() > 0)) {
      status = Status::error(StatusCode::Internal, "Library: HasOverrides is out of date");
    }
  });
  return status;
}

} // namespace coredcc

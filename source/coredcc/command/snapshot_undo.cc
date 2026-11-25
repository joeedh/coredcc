#include "coredcc/command/snapshot_undo.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/lib/io/format.h"
#include "coredcc/lib/library.h"

#include <utility>

namespace coredcc {

using litestl::util::Vector;

/* ---- SnapshotRegistry ---- */

SnapshotRegistry::~SnapshotRegistry()
{
  for (Entry *e : entries_) {
    litestl::alloc::Delete(e);
  }
}

Status SnapshotRegistry::add(std::string_view name, Owned<SnapshotProvider> provider)
{
  const std::string what = "snapshot provider '" + std::string(name) + "'";
  if (name.empty() || !provider) {
    return Status::error(StatusCode::InvalidArgument,
                         (what + " has no name or no implementation").c_str());
  }
  if (find(name)) {
    return Status::error(StatusCode::AlreadyExists, (what + " is already registered").c_str());
  }
  Entry *e = litestl::alloc::New<Entry>("coredcc::SnapshotRegistry::Entry");
  e->name = name;
  e->provider = std::move(provider);
  entries_.append(e);
  return Status();
}

Status SnapshotRegistry::remove(std::string_view name)
{
  for (int i = 0; i < int(entries_.size()); i++) {
    if (entries_[i]->name == name) {
      Entry *e = entries_[i];
      entries_.remove_at(i);
      litestl::alloc::Delete(e);
      return Status();
    }
  }
  return Status::error(StatusCode::NotFound,
                       ("no snapshot provider '" + std::string(name) + "'").c_str());
}

SnapshotProvider *SnapshotRegistry::find(std::string_view name)
{
  for (Entry *e : entries_) {
    if (e->name == name) {
      return &e->provider.get();
    }
  }
  return nullptr;
}

/* ---- SnapshotUndo ---- */

namespace {

Status noProvider(const std::string &name)
{
  return Status::error(StatusCode::NotFound,
                       ("no snapshot provider '" + name + "' is registered").c_str());
}

Status notTaken(const char *what)
{
  return Status::error(StatusCode::FailedPrecondition,
                       (std::string(what) + ": no snapshot was taken").c_str());
}

} // namespace

Status SnapshotUndo::capture(Context &ctx, Blob &to)
{
  Runtime &rt = ctx.runtime();
  SnapshotProvider *found = rt.ext<SnapshotRegistry>().find(name_);
  if (!found) {
    return noProvider(name_);
  }
  const SnapshotProvider p = *found;
  Vector<uint8_t> raw;
  Status taken = p.capture(ctx, raw);
  if (rt.hasStickyError()) {
    return rt.takeStickyError(); /* a foreign provider threw */
  }
  CDCC_TRY(taken);
  Blob blob;
  if (Library *lib = ctx.library(); lib && p.hasForeachRef()) {
    struct Pinner {
      Library &lib;
      litestl::util::Map<BlockHandle, bool> seen;
      Vector<HistoryBlock> &to;
    } pinner{*lib, {}, blob.targets};
    RefVisitor v(
        [](void *self, RefCell &cell) {
          Pinner &p = *static_cast<Pinner *>(self);
          const BlockHandle h = p.lib.find(cell.target);
          if (h && !p.seen.lookup_ptr(h)) {
            p.seen.add(h, true);
            p.to.append(HistoryBlock(p.lib, h));
          }
        },
        &pinner);
    p.foreachRef(ctx, raw, v);
  }
  if (rt.hasStickyError()) {
    return rt.takeStickyError();
  }
  if (uint64_t(raw.size()) > detail::libio::kMaxRaw) {
    return Status::error(StatusCode::OutOfRange, "the snapshot is too large");
  }
  detail::libio::Stored s =
      detail::libio::pack(std::span<const uint8_t>(raw.data(), size_t(raw.size())), true);
  blob.taken = true;
  blob.lz4 = s.lz4;
  blob.raw = s.raw;
  blob.bytes = std::move(s.bytes);
  to = std::move(blob);
  return Status();
}

Status SnapshotUndo::restore(Context &ctx, const Blob &from)
{
  Runtime &rt = ctx.runtime();
  SnapshotProvider *found = rt.ext<SnapshotRegistry>().find(name_);
  if (!found) {
    return noProvider(name_);
  }
  Vector<uint8_t> raw;
  detail::libio::Budget unbounded{UINT64_MAX};
  CDCC_TRY(detail::libio::unpack(
      std::span<const uint8_t>(from.bytes.data(), size_t(from.bytes.size())), from.lz4, from.raw,
      unbounded, raw));
  SnapshotProvider p = *found;
  Status s = p.restore(ctx, raw);
  if (rt.hasStickyError()) {
    Status sticky = rt.takeStickyError();
    s = s.ok() ? sticky : s;
  }
  if (Library *lib = ctx.library()) {
    lib->noteRefsChanged();
  }
  rt.ext<ChangeBus>().notifyAll();
  return s;
}

Status SnapshotUndo::begin(Context &ctx)
{
  after_ = Blob();
  before_ = Blob();
  restoreAtDiscard_ = false;
  rt_ = &ctx.runtime();
  lib_ = ctx.library() ? ctx.library()->ref() : LibraryRef();
  root_ = ctx.root();
  return capture(ctx, before_);
}

Status SnapshotUndo::end(Context &ctx)
{
  return before_.taken ? capture(ctx, after_) : notTaken("end");
}

void SnapshotUndo::undo(Context &ctx)
{
  Status s = before_.taken ? restore(ctx, before_) : notTaken("undo");
  if (!s.ok()) {
    ctx.runtime().setStickyError(std::move(s));
  }
}

Status SnapshotUndo::redo(Context &ctx)
{
  return after_.taken ? restore(ctx, after_) : notTaken("redo");
}

Status SnapshotUndo::rollback(Context &ctx)
{
  if (!before_.taken) {
    return notTaken("rollback");
  }
  restoreAtDiscard_ = true;
  return restore(ctx, before_);
}

bool SnapshotUndo::foldFrom(SnapshotUndo &next)
{
  if (name_ != next.name_ || !after_.taken || !next.after_.taken) {
    return false;
  }
  after_ = std::move(next.after_);
  next.after_ = Blob();
  return true;
}

uint64_t SnapshotUndo::memSize() const
{
  return sizeof(*this) + uint64_t(before_.bytes.size()) + uint64_t(after_.bytes.size()) +
         (uint64_t(before_.targets.size()) + uint64_t(after_.targets.size())) * sizeof(HistoryBlock);
}

void SnapshotUndo::discard()
{
  if (std::exchange(restoreAtDiscard_, false)) {
    Context ctx(rt_, lib_.get(), root_);
    if (Status s = restore(ctx, before_); !s.ok()) {
      rt_->setStickyError(std::move(s));
    }
  }
  before_ = Blob();
  after_ = Blob();
}

} // namespace coredcc

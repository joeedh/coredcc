#include "coredcc/scene/commands.h"

#include "coredcc/command/history_block.h"

#include <cstring>

namespace coredcc {

Status ObjectChanges::apply(Context &ctx, const Change &c, const StoredObject &values)
{
  Library &lib = *ctx.library();
  const BlockHandle h = lib.find(c.object);
  const Datablock *b = lib.get(h);
  detail::PendingWrites &pending = ctx.runtime().ext<detail::PendingWrites>();
  if (b && b->state() != BlockState::Live) {
    pending.add(lib, c.object, c.fields, values);
  }
  if (!b || b->state() != BlockState::Live) {
    return Status(); /* without a record it is gone for good */
  }
  pending.flush(lib, c.object); /* older */
  return detail::writeStored(ctx, h, c.fields, values);
}

Status ObjectChanges::write(Context &ctx, BlockHandle object, const Values &values)
{
  if (changes_.size() == 0) {
    lib_ = ctx.library()->ref();
  }
  CDCC_TRY(detail::libraryOf(ctx, lib_).status());
  Result<ObjectRead> r = readObject(*ctx.library(), object);
  Status s = r.status();
  if (r.ok()) {
    Change c;
    c.object = r->owner.block;
    c.before.channels = r->channels;
    c.before.type = r->info.transformType;
    c.before.parent = r->info.parent;
    c.before.flags = ObjectFlags(r->info.flags);
    c.before.locks = r->info.locks;
    c.after = c.before;
    c.fields.channels = values.channels != nullptr;
    c.fields.type = values.type != nullptr;
    c.fields.parent = values.parent != nullptr;
    c.fields.flags = values.flags != nullptr;
    c.fields.locks = values.locks != nullptr || values.type != nullptr;
    if (values.channels) {
      c.after.channels = *values.channels;
    }
    if (values.type) {
      c.after.type = *values.type;
      c.after.locks = 0;
    }
    if (values.parent) {
      c.after.parent = *values.parent;
    }
    if (values.flags) {
      c.after.flags = *values.flags;
    }
    if (values.locks) {
      c.after.locks = *values.locks;
    }
    s = apply(ctx, c, c.after);
    if (s.ok()) {
      changes_.append(std::move(c));
      return s;
    }
    apply(ctx, c, c.before).ignore();
  }
  takeBack(ctx).ignore();
  changes_.clear();
  return detail::about(*ctx.library(), object, s);
}

Status ObjectChanges::rewrite(Context &ctx, int i, const ChannelBlock &channels)
{
  CDCC_TRY(detail::libraryOf(ctx, lib_).status());
  StoredObject after = changes_[i].after;
  after.channels = channels;
  CDCC_TRY(apply(ctx, changes_[i], after));
  changes_[i].after.channels = channels;
  return Status();
}

/* The first error; the rest is still written. */
Status ObjectChanges::takeBack(Context &ctx)
{
  Status first;
  for (int i = int(changes_.size()) - 1; i >= 0; i--) {
    if (Status s = apply(ctx, changes_[i], changes_[i].before); !s.ok() && first.ok()) {
      first = std::move(s);
    }
  }
  return first;
}

void ObjectChanges::undo(Context &ctx)
{
  Runtime &rt = ctx.runtime();
  /* A sub-command's undo may have left one; writing needs it out of the way. */
  Status pending = rt.hasStickyError() ? rt.takeStickyError() : Status();
  Status s = changes_.size() == 0 ? Status() : detail::libraryOf(ctx, lib_).status();
  if (s.ok()) {
    s = takeBack(ctx);
  }
  if (!pending.ok()) {
    rt.setStickyError(std::move(pending));
  } else if (!s.ok()) {
    rt.setStickyError(std::move(s));
  }
}

Status ObjectChanges::redo(Context &ctx)
{
  if (changes_.size() > 0) {
    CDCC_TRY(detail::libraryOf(ctx, lib_).status());
  }
  for (int i = 0; i < int(changes_.size()); i++) {
    if (Status s = apply(ctx, changes_[i], changes_[i].after); !s.ok()) {
      for (int j = i; j >= 0; j--) {
        apply(ctx, changes_[j], changes_[j].before).ignore();
      }
      return s;
    }
  }
  return Status();
}

static bool sameChannels(const ChannelBlock &a, const ChannelBlock &b)
{
  bool same = a.size() == b.size();
  for (int32_t i = 0; same && i < a.size(); i++) {
    const double x = a.get(i), y = b.get(i);
    same = memcmp(&x, &y, sizeof(x)) == 0;
  }
  return same;
}

bool ObjectChanges::foldFrom(const ObjectChanges &next)
{
  bool same = changes_.size() == next.changes_.size() && lib_.get() == next.lib_.get();
  for (int i = 0; same && i < int(changes_.size()); i++) {
    const Change &a = changes_[i], &b = next.changes_[i];
    same = a.object == b.object && a.fields == b.fields &&
           a.after.type == b.before.type && a.after.parent == b.before.parent &&
           a.after.flags == b.before.flags && a.after.locks == b.before.locks &&
           sameChannels(a.after.channels, b.before.channels);
  }
  for (int i = 0; same && i < int(changes_.size()); i++) {
    changes_[i].after = next.changes_[i].after;
  }
  return same;
}

bool ObjectChanges::channelsChanged() const
{
  for (const Change &c : changes_) {
    if (c.fields.channels && !sameChannels(c.before.channels, c.after.channels)) {
      return true;
    }
  }
  return false;
}

uint64_t ObjectChanges::byteSize() const
{
  uint64_t n = uint64_t(changes_.size()) * sizeof(Change);
  for (const Change &c : changes_) {
    n += uint64_t(c.before.channels.size() + c.after.channels.size()) * sizeof(double);
  }
  return n;
}

} // namespace coredcc

#include "coredcc/scene/commands.h"

#include "coredcc/command/history_block.h"

namespace coredcc {

/* A parent before the rest, and a type with its channels. */
Status ObjectChanges::apply(Context &ctx, const Change &c, const Stored &values)
{
  const BlockHandle h = ctx.library()->find(c.object);
  if (c.hasParent) {
    CDCC_TRY(writeParent(ctx, h, values.parent));
  }
  if (c.hasType) {
    return writeTransformType(ctx, h, values.type, values.channels);
  }
  return c.hasChannels ? writeTransform(ctx, h, values.channels) : Status();
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
    c.after = c.before;
    c.hasChannels = values.channels != nullptr;
    c.hasType = values.type != nullptr;
    c.hasParent = values.parent != nullptr;
    if (values.channels) {
      c.after.channels = *values.channels;
    }
    if (values.type) {
      c.after.type = *values.type;
    }
    if (values.parent) {
      c.after.parent = *values.parent;
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
  Stored after = changes_[i].after;
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
  }
  else if (!s.ok()) {
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

uint64_t ObjectChanges::memSize() const
{
  uint64_t n = uint64_t(changes_.size()) * sizeof(Change);
  for (const Change &c : changes_) {
    n += uint64_t(c.before.channels.size() + c.after.channels.size()) * sizeof(double);
  }
  return n;
}

} // namespace coredcc

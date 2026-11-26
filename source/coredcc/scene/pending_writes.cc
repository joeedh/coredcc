#include "coredcc/scene/pending_writes.h"

#include "coredcc/api/context.h"
#include "coredcc/scene/object.h"

#include <utility>

namespace coredcc::detail {

Status writeStored(Context &ctx,
                   BlockHandle h,
                   const StoredFields &fields,
                   const StoredObject &v)
{
  if (fields.parent) {
    CDCC_TRY(writeParent(ctx, h, v.parent));
  }
  if (fields.flags) {
    CDCC_TRY(writeFlags(ctx, h, v.flags));
  }
  if (fields.type) {
    CDCC_TRY(writeTransformType(ctx, h, v.type, v.channels));
  } else if (fields.channels) {
    CDCC_TRY(writeTransform(ctx, h, v.channels));
  }
  if (!fields.locks) {
    return Status();
  }
  /* A type comes without locks, and an object may not be able to have any. */
  Result<ObjectRead> r = readObject(*ctx.library(), h);
  return r.ok() && r->info.locks == v.locks ? Status() : writeLocks(ctx, h, v.locks);
}

PendingWrites::PerLibrary *PendingWrites::find(const Library &lib)
{
  for (PerLibrary &p : libraries_) {
    if (p.lib.get() == &lib) {
      return &p;
    }
  }
  return nullptr;
}

void PendingWrites::add(Library &lib,
                        const BlockId &object,
                        const StoredFields &fields,
                        const StoredObject &v)
{
  PerLibrary *p = find(lib);
  if (!p) {
    litestl::util::Vector<PerLibrary> alive;
    for (PerLibrary &x : libraries_) {
      if (x.lib.get()) {
        alive.append(std::move(x));
      }
    }
    libraries_ = std::move(alive);
    PerLibrary mine;
    mine.lib = lib.ref();
    libraries_.append(std::move(mine));
    p = &libraries_[int(libraries_.size()) - 1];
    lib.addReviveListener(
        [this](Library &l, BlockHandle h) { flush(l, l.get(h)->id()); });
  }
  Entry *e = nullptr;
  for (Entry &x : p->entries) {
    e = x.object == object ? &x : e;
  }
  if (!e) {
    p->entries.append(Entry{object, {}, {}});
    e = &p->entries[int(p->entries.size()) - 1];
  }
  if (fields.parent) {
    e->fields.parent = true;
    e->values.parent = v.parent;
  }
  if (fields.type) {
    e->fields.type = true;
    e->values.type = v.type;
  }
  if (fields.flags) {
    e->fields.flags = true;
    e->values.flags = v.flags;
  }
  if (fields.locks) {
    e->fields.locks = true;
    e->values.locks = v.locks;
  }
  if (fields.type || fields.channels) {
    e->fields.channels = true;
    e->values.channels = v.channels;
  }
}

void PendingWrites::flush(Library &lib, const BlockId &object)
{
  Runtime &rt = lib.runtime();
  PerLibrary *p = find(lib);
  if (!p || rt.hasStickyError() || !rt.busyIfEvaluating("").ok() || lib.busy()) {
    return; /* evaluateScene tries again */
  }
  const BlockHandle h = lib.find(object);
  const Datablock *b = lib.get(h);
  if (b && b->state() != BlockState::Live) {
    return;
  }
  for (int i = 0; i < int(p->entries.size()); i++) {
    if (!(p->entries[i].object == object)) {
      continue;
    }
    const Entry e = std::move(p->entries[i]);
    p->entries.remove_at(i);
    Context ctx(&rt, &lib, PointerRef());
    Status s = b ? writeStored(ctx, h, e.fields, e.values) : Status();
    if (!s.ok() && !rt.hasStickyError()) {
      rt.setStickyError(std::move(s));
    }
    return;
  }
}

void PendingWrites::flush(Library &lib)
{
  litestl::util::Vector<BlockId> ids;
  if (const PerLibrary *p = find(lib)) {
    for (const Entry &e : p->entries) {
      ids.append(e.object);
    }
  }
  for (const BlockId &id : ids) {
    flush(lib, id);
  }
}

int PendingWrites::writeCount(const Library &lib) const
{
  const PerLibrary *p = const_cast<PendingWrites *>(this)->find(lib);
  return p ? int(p->entries.size()) : 0;
}

} // namespace coredcc::detail

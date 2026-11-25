#include "coredcc/api/resolve.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/base/text.h"
#include "coredcc/lib/library.h"
#include "coredcc/parse/path_cache.h"

#include <cstring>
#include <string>

namespace coredcc {

using litestl::util::Vector;

Status detail::atSegment(const PathSegment &seg, const Status &s)
{
  const std::string msg =
      std::string(s.message()) + " (at offset " + std::to_string(seg.offset) + ")";
  Status out = Status::error(s.code(), msg.c_str());
  for (const litestl::util::string &sg : s.suggestions()) {
    out.suggest(sg.c_str());
  }
  return out;
}

namespace {

Status at(const PathSegment &seg, const Status &s)
{
  return detail::atSegment(seg, s);
}

Status err(StatusCode code, const std::string &msg)
{
  return Status::error(code, msg.c_str());
}

PointerRef blockPointer(Context &ctx, const OwnerKey &owner);

/* The struct an owner's keys start from: a block's data, or a runtime owner. */
PointerRef ownerRoot(Context &ctx, const ApiRegistry &reg, const OwnerKey &owner)
{
  return owner.kind == OwnerKey::Kind::Block ? blockPointer(ctx, owner) : reg.runtimeOwner(owner);
}

/* The walk shared by paths and StableRefs: the current struct, its owner and the key
 * from that owner, and what the resolution touched. */
struct Walk {
  Context &ctx;
  ApiRegistry &reg;
  PointerRef cur;
  OwnerKey owner;
  Vector<KeySegment, 2> key;
  ResolvedPath out;
  /** A trailing `.active` names the collection's activeMember. */
  bool forWrite = false;

  /* Moves into `next`, reached through member `m` (and an item key). Canonical paths
   * keep no hops that can be reassigned:
   * - crossing into another owner restarts the key there, so `next` must be that
   *   owner's root struct (its type comes from the owner when unset);
   * - a settable pointer inside one owner restarts from its `address`;
   * - collection items and fixed pointers append to the key. */
  Status enter(PointerRef next, const ApiMember &m, const KeySegment *itemKey)
  {
    if (!next.owner.isNull() && !(next.owner == owner)) {
      const PointerRef root = ownerRoot(ctx, reg, next.owner);
      if (root.isNull() || root.data != next.data) {
        return err(StatusCode::Unsupported,
                   std::string("'") + m.name() +
                       "' returned a struct of another owner that isn't the owner's root; "
                       "return the root, or leave the owner unset");
      }
      if (!next.type) {
        next.type = root.type;
      }
      owner = next.owner;
      key.clear();
    }
    else if (m.kind() == MemberKind::Pointer && m.pointer().address) {
      StableRef a = m.pointer().address(cur, ctx, next);
      if (a.owner.isNull()) {
        return err(StatusCode::Internal,
                   std::string("pointer '") + m.name() + "': its address callback gave no owner");
      }
      owner = a.owner;
      key.clear();
      for (const KeySegment &k : a.key) {
        key.append(k);
      }
    }
    else if (m.kind() == MemberKind::Pointer && m.pointer().set) {
      return err(StatusCode::Unsupported,
                 std::string("pointer '") + m.name() +
                     "' is settable and points inside its own owner: give it an address "
                     "callback, so paths through it don't depend on where it points later");
    }
    else {
      key.append(KeySegment::member(m.name()));
      if (itemKey) {
        key.append(*itemKey);
      }
    }
    next.owner = owner;
    cur = next;
    return Status();
  }

  void touch(const char *member)
  {
    out.touched.append(PathTouch{owner, lsString(member)});
  }

  ResolvedPath finish(ResolvedPath::Kind kind, const ApiMember *m)
  {
    out.kind = kind;
    out.ptr = cur;
    out.member = m;
    out.canonical.owner = owner;
    for (const KeySegment &k : key) {
      out.canonical.key.append(k);
    }
    if (m) {
      out.canonical.key.append(KeySegment::member(m->name()));
    }
    else {
      out.canonical.type = cur.type;
    }
    return std::move(out);
  }
};

StructId pointerTargetType(const ApiMember &m, const PointerRef &target)
{
  if (target.type) {
    return target.type;
  }
  if (m.pointer().target) {
    return m.pointer().target;
  }
  return m.pointer().refine ? m.pointer().refine(target) : StructId();
}

const EnumItem *findItem(const ApiMember &m, std::string_view key)
{
  const EnumMeta *em = m.def().enumMeta();
  return em ? em->findKey(key) : nullptr;
}

/* With `through`, a path that ends at a Pointer member is its target. */
Result<ResolvedPath> walkPath(Walk &w, const Path &path, bool through = false)
{
  const int n = path.size();
  int i = 0;
  while (i < n) {
    const PathSegment &seg = path[i];
    if (seg.kind != SegmentKind::Member) {
      return at(seg, err(StatusCode::InvalidArgument, "a subscript needs a collection or vector"));
    }
    const ApiMember *m = w.reg.findMember(w.cur.type, seg.text.c_str());
    if (!m) {
      return at(seg, w.reg.unknownMember(w.cur.type, seg.text.c_str()));
    }
    w.touch(m->name());
    const bool last = i + 1 == n;
    switch (m->kind()) {
      case MemberKind::Value: {
        if (last) {
          return w.finish(ResolvedPath::Kind::Member, m);
        }
        const PathSegment &sub = path[i + 1];
        if (i + 2 != n) {
          return at(path[i + 2], err(StatusCode::InvalidArgument,
                                     std::string("nothing can follow '") + m->name() +
                                         "[...]'"));
        }
        const int comps = componentCount(m->def().kind());
        if (sub.kind == SegmentKind::Index && comps > 1) {
          if (sub.index >= comps) {
            return at(sub, err(StatusCode::OutOfRange,
                               std::string("'") + m->name() + "' has " +
                                   std::to_string(comps) + " components"));
          }
          w.out.component = int(sub.index);
          return w.finish(ResolvedPath::Kind::Component, m);
        }
        if (sub.kind == SegmentKind::EnumKey &&
            (m->def().kind() == PropKind::Enum || m->def().kind() == PropKind::Flags))
        {
          if (!findItem(*m, sub.text.c_str())) {
            return at(sub, err(StatusCode::NotFound,
                               std::string("'") + m->name() + "' has no key '" +
                                   sub.text.c_str() + "'"));
          }
          w.out.enumKey = sub.text;
          return w.finish(ResolvedPath::Kind::EnumKey, m);
        }
        return at(sub, err(StatusCode::InvalidArgument,
                           std::string("'") + m->name() + "' (" + propKindName(m->def().kind()) +
                               ") has no members or components"));
      }
      case MemberKind::Pointer: {
        if (last && !through) {
          return w.finish(ResolvedPath::Kind::Member, m);
        }
        PointerRef target = m->pointer().get(w.cur, w.ctx);
        if (target.isNull()) {
          return at(path[last ? i : i + 1],
                    err(StatusCode::NotFound,
                        std::string("pointer '") + m->name() + "' is null"));
        }
        if (Status s = w.enter(target, *m, nullptr); !s.ok()) {
          return at(seg, s);
        }
        if (!w.cur.type) {
          w.cur.type = pointerTargetType(*m, w.cur);
        }
        i++;
        break;
      }
      case MemberKind::Collection: {
        if (last) {
          return w.finish(ResolvedPath::Kind::Member, m);
        }
        const CollectionAccessor &c = m->collection();
        const PathSegment &sub = path[i + 1];
        PointerRef item;
        if (sub.kind == SegmentKind::Member && strcmp(sub.text.c_str(), "length") == 0) {
          if (i + 2 != n) {
            return at(path[i + 2], err(StatusCode::InvalidArgument,
                                       "nothing can follow '.length'"));
          }
          return w.finish(ResolvedPath::Kind::Length, m);
        }
        if (sub.kind == SegmentKind::Member && strcmp(sub.text.c_str(), "active") == 0) {
          if (!c.active) {
            return at(sub, err(StatusCode::Unsupported,
                               std::string("'") + m->name() + "' has no active item"));
          }
          if (c.activeMember.size() > 0) {
            w.touch(c.activeMember.c_str()); /* a write to it changes what this path means */
            if (w.forWrite && i + 2 == n) {
              const ApiMember *am = w.reg.findMember(w.cur.type, c.activeMember.c_str());
              if (!am || am->kind() != MemberKind::Pointer) {
                return at(sub, err(StatusCode::Internal,
                                   std::string("'") + m->name() + "': its activeMember '" +
                                       c.activeMember.c_str() + "' isn't a pointer member"));
              }
              return w.finish(ResolvedPath::Kind::Member, am);
            }
          }
          item = c.active(w.cur, w.ctx);
        }
        else if (sub.kind == SegmentKind::Index) {
          const int64_t len = c.length(w.cur, w.ctx);
          if (sub.index >= len) {
            return at(sub, err(StatusCode::OutOfRange,
                               std::string("index ") + std::to_string(sub.index) + " past '" +
                                   m->name() + "' (length " + std::to_string(len) + ")"));
          }
          item = c.at(w.cur, w.ctx, sub.index);
        }
        else if (sub.kind == SegmentKind::StringKey) {
          item = c.lookup(w.cur, w.ctx, KeySegment::stringKey(sub.text.c_str()));
        }
        else if (sub.kind == SegmentKind::HandleKey) {
          item = c.lookup(w.cur, w.ctx, KeySegment::ofHandle(sub.index));
        }
        else if (sub.kind == SegmentKind::Member) {
          Status s = err(StatusCode::NotFound, std::string("collection '") + m->name() +
                                                   "' has no member '" + sub.text.c_str() +
                                                   "' (only .active and .length)");
          Suggester sg(sub.text.c_str());
          sg.consider("active");
          sg.consider("length");
          sg.addTo(s);
          return at(sub, s);
        }
        else {
          return at(sub, err(StatusCode::InvalidArgument,
                             std::string("'") + m->name() +
                                 "' takes [index], [\"key\"], [#handle], .active or .length"));
        }
        if (item.isNull()) {
          return at(sub, err(StatusCode::NotFound,
                             std::string("no such item in '") + m->name() + "'"));
        }
        const KeySegment k = c.keyOf(w.cur, w.ctx, item);
        if (k.kind == KeySegment::Kind::Member) {
          return at(sub, err(StatusCode::Internal,
                             std::string("collection '") + m->name() +
                                 "': keyOf must return a string key or a handle"));
        }
        if (Status s = w.enter(item, *m, &k); !s.ok()) {
          return at(sub, s);
        }
        if (!w.cur.type) {
          w.cur.type = c.itemStruct;
        }
        i += 2;
        break;
      }
    }
  }
  return w.finish(ResolvedPath::Kind::Struct, nullptr);
}

Result<Walk *> startAtRoot(Context &ctx, Walk &w)
{
  w.cur = ctx.root();
  w.owner = ctx.root().owner;
  if (ctx.hasPrefix()) {
    Walk pw{ctx, w.reg, ctx.root(), ctx.root().owner, {}, {}};
    Result<ResolvedPath> base = ctx.prefix() ? walkPath(pw, *ctx.prefix(), true) :
                                               resolve(ctx, *ctx.prefixAddress());
    if (!base.ok()) {
      return base.status();
    }
    if (base->kind != ResolvedPath::Kind::Struct) {
      return err(StatusCode::InvalidArgument, "the context prefix must name a struct");
    }
    w.cur = base->ptr;
    w.owner = base->canonical.owner;
    w.key = base->canonical.key;
    w.out.touched = base->touched;
  }
  return &w;
}

PointerRef blockPointer(Context &ctx, const OwnerKey &owner)
{
  Library *lib = ctx.library();
  if (!lib || owner.kind != OwnerKey::Kind::Block) {
    return PointerRef();
  }
  const BlockHandle h = lib->find(owner.block);
  const Datablock *b = lib->get(h);
  if (!b) {
    return PointerRef();
  }
  const BlockTypeRegistry::Entry *e = ctx.runtime().ext<BlockTypeRegistry>().find(b->type());
  void *data = lib->data(h); /* null unless Live */
  if (!e || !data) {
    return PointerRef();
  }
  return PointerRef{data, e->info.dataStruct, owner};
}

} // namespace

namespace {

Result<ResolvedPath> resolvePath(Context &ctx, const Path &path, bool forWrite)
{
  Walk w{ctx, ctx.runtime().ext<ApiRegistry>(), {}, {}, {}, {}};
  Result<Walk *> started = startAtRoot(ctx, w);
  if (!started.ok()) {
    return started.status();
  }
  w.forWrite = forWrite;
  return walkPath(w, path);
}

Result<ResolvedPath> resolvePath(Context &ctx, std::string_view path, bool forWrite)
{
  Result<PathPtr> p = ctx.runtime().ext<PathCache>().get(path);
  if (!p.ok()) {
    return p.status();
  }
  return resolvePath(ctx, **p, forWrite);
}

} // namespace

Result<ResolvedPath> resolve(Context &ctx, const Path &path)
{
  return resolvePath(ctx, path, false);
}

Result<ResolvedPath> resolve(Context &ctx, std::string_view path)
{
  return resolvePath(ctx, path, false);
}

Result<ResolvedPath> resolveForWrite(Context &ctx, const Path &path)
{
  return resolvePath(ctx, path, true);
}

Result<ResolvedPath> resolveForWrite(Context &ctx, std::string_view path)
{
  return resolvePath(ctx, path, true);
}

Result<ResolvedPath> resolve(Context &ctx, const StableRef &ref)
{
  if (ref.isNull()) {
    return err(StatusCode::InvalidArgument, "resolve: a null StableRef");
  }
  ApiRegistry &reg = ctx.runtime().ext<ApiRegistry>();
  PointerRef start = ref.owner.kind == OwnerKey::Kind::Block ? blockPointer(ctx, ref.owner) :
                                                               reg.runtimeOwner(ref.owner);
  if (start.isNull()) {
    return err(StatusCode::NotFound,
               "resolve: the owner is gone (deleted, detached, or not registered): " +
                   ref.toString());
  }
  Walk w{ctx, reg, start, ref.owner, {}, {}};
  const int n = int(ref.key.size());
  for (int j = 0; j < n; j++) {
    const KeySegment &seg = ref.key[j];
    if (seg.kind != KeySegment::Kind::Member) {
      return err(StatusCode::InvalidArgument, "resolve: a key where a member was expected");
    }
    const ApiMember *m = reg.findMember(w.cur.type, seg.text.c_str());
    if (!m) {
      return reg.unknownMember(w.cur.type, seg.text.c_str());
    }
    w.touch(m->name());
    const bool last = j + 1 == n;
    if (m->kind() == MemberKind::Value || (last && m->kind() == MemberKind::Collection) ||
        (last && m->kind() == MemberKind::Pointer && !ref.type))
    {
      if (!last) {
        return err(StatusCode::InvalidArgument,
                   std::string("resolve: '") + m->name() + "' has no members");
      }
      return w.finish(ResolvedPath::Kind::Member, m);
    }
    if (m->kind() == MemberKind::Pointer) {
      /* A trailing pointer is followed when the ref addresses a struct (has a type). */
      PointerRef target = m->pointer().get(w.cur, ctx);
      if (target.isNull()) {
        return err(StatusCode::NotFound, std::string("resolve: pointer '") + m->name() + "' is null");
      }
      CDCC_TRY(w.enter(target, *m, nullptr));
      if (!w.cur.type) {
        w.cur.type = pointerTargetType(*m, w.cur);
      }
      continue;
    }
    if (j + 1 >= n || ref.key[j + 1].kind == KeySegment::Kind::Member) {
      return err(StatusCode::InvalidArgument,
                 std::string("resolve: collection '") + m->name() + "' needs an item key");
    }
    const KeySegment &k = ref.key[j + 1];
    PointerRef item = m->collection().lookup(w.cur, ctx, k);
    if (item.isNull()) {
      return err(StatusCode::NotFound,
                 std::string("resolve: no item with that key in '") + m->name() + "'");
    }
    CDCC_TRY(w.enter(item, *m, &k));
    if (!w.cur.type) {
      w.cur.type = m->collection().itemStruct;
    }
    j++;
  }
  if (ref.type && !reg.isA(w.cur.type, ref.type)) {
    return err(StatusCode::TypeMismatch, "resolve: the target is of another struct type");
  }
  return w.finish(ResolvedPath::Kind::Struct, nullptr);
}

Result<StableRef> canonicalize(Context &ctx, const Path &path)
{
  Result<ResolvedPath> rp = resolve(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  if (rp->canonical.owner.isNull()) {
    return err(StatusCode::FailedPrecondition,
               "canonicalize: the path has no owner (the context root needs one)");
  }
  return std::move(rp->canonical);
}

/* ---- reading ---- */

namespace {

/* The member's def with both dynamic hooks applied. */
PropertyDef effectiveDef(Context &ctx, const ResolvedPath &rp)
{
  PropertyDef def = rp.member->def().isDynamic() ? rp.member->def().resolved(rp.ptr.data) :
                                                   rp.member->def();
  if (rp.member->dynamicFn()) {
    rp.member->dynamicFn()(def, rp.ptr, ctx);
  }
  return def;
}

/* The address a Pointer value stores for `target`: never a hop that can be reassigned
 * (see Walk::enter). */
Result<StableRef> addressOf(Context &ctx, const ResolvedPath &rp, const PointerRef &target)
{
  const ApiMember &m = *rp.member;
  const ApiRegistry &reg = ctx.runtime().ext<ApiRegistry>();
  StableRef r;
  if (!target.owner.isNull() && !(target.owner == rp.canonical.owner)) {
    const PointerRef root = ownerRoot(ctx, reg, target.owner);
    if (root.isNull() || root.data != target.data) {
      return err(StatusCode::Unsupported,
                 std::string("pointer '") + m.name() +
                     "' targets a struct of another owner that isn't the owner's root");
    }
    r.owner = target.owner;
    r.type = target.type ? target.type : root.type;
    return r;
  }
  if (m.pointer().address) {
    r = m.pointer().address(rp.ptr, ctx, target);
    if (r.owner.isNull()) {
      return err(StatusCode::Internal,
                 std::string("pointer '") + m.name() + "': its address callback gave no owner");
    }
  }
  else if (m.pointer().set) {
    return err(StatusCode::Unsupported,
               std::string("pointer '") + m.name() +
                   "' is settable and points inside its own owner: it needs an address "
                   "callback to give its target a stable address");
  }
  else {
    r.owner = rp.canonical.owner; /* a fixed sub-struct: the hop is stable */
    r.key = rp.canonical.key;
  }
  r.type = pointerTargetType(m, target);
  return r;
}

/* A value member's whole value, checked against its def: a getter returning another
 * type (or a failed foreign call's default) is an error, not an abort later. */
Result<PropValue> readMember(Context &ctx, const ResolvedPath &rp)
{
  const ApiMember &m = *rp.member;
  PropValue v = m.value().get(rp.ptr, ctx);
  const PropValue::Type want = valueTypeFor(m.def().kind());
  if (v.type() != want) {
    return err(StatusCode::Internal, std::string("the getter of '") + m.name() + "' returned a " +
                                         propValueTypeName(v.type()) + ", not a " +
                                         propValueTypeName(want));
  }
  return v;
}

} // namespace

Result<PropValue> getValue(Context &ctx, const ResolvedPath &rp)
{
  switch (rp.kind) {
    case ResolvedPath::Kind::Struct:
      return PropValue::ofPointer(rp.canonical);
    case ResolvedPath::Kind::Length:
      return PropValue::ofInt(rp.member->collection().length(rp.ptr, ctx));
    default:
      break;
  }
  const ApiMember &m = *rp.member;
  if (m.kind() == MemberKind::Collection) {
    return err(StatusCode::Unsupported,
               std::string("'") + m.name() + "' is a collection: read .length or an item");
  }
  if (m.kind() == MemberKind::Pointer) {
    PointerRef target = m.pointer().get(rp.ptr, ctx);
    if (target.isNull()) {
      return PropValue::nullPointer();
    }
    Result<StableRef> addr = addressOf(ctx, rp, target);
    if (!addr.ok()) {
      return addr.status();
    }
    return PropValue::ofPointer(*addr);
  }
  Result<PropValue> read = readMember(ctx, rp);
  if (!read.ok()) {
    return read.status();
  }
  const PropValue &v = *read;
  if (rp.kind == ResolvedPath::Kind::Component) {
    return PropValue::ofFloat(v.component(rp.component));
  }
  if (rp.kind == ResolvedPath::Kind::EnumKey) {
    const EnumItem *item = findItem(m, rp.enumKey.c_str());
    if (v.type() == PropValue::Type::Flags) {
      return PropValue::ofBool((v.asFlags() & uint64_t(item->value)) != 0);
    }
    return PropValue::ofBool(v.asEnum() == item->value);
  }
  return v;
}

Result<PropValue> getValue(Context &ctx, std::string_view path)
{
  Result<ResolvedPath> rp = resolve(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  return getValue(ctx, *rp);
}

Result<PropertyDef> getMeta(Context &ctx, const ResolvedPath &rp)
{
  switch (rp.kind) {
    case ResolvedPath::Kind::Struct:
      return err(StatusCode::InvalidArgument, "getMeta: the path names a struct");
    case ResolvedPath::Kind::Length:
      return PropertyDef::intProp("length").access(PropAccess::ReadOnly);
    case ResolvedPath::Kind::EnumKey: {
      PropertyDef d = PropertyDef::boolProp(rp.enumKey.c_str());
      if (rp.member->readOnly() || effectiveDef(ctx, rp).hasAccess(PropAccess::ReadOnly)) {
        d.access(PropAccess::ReadOnly);
      }
      return d;
    }
    default:
      break;
  }
  PropertyDef def = effectiveDef(ctx, rp);
  if (rp.member->readOnly()) {
    def.access(PropAccess::ReadOnly);
  }
  return def;
}

Result<PropertyDef> getMeta(Context &ctx, std::string_view path)
{
  Result<ResolvedPath> rp = resolve(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  return getMeta(ctx, *rp);
}

/* ---- writing ---- */

namespace {

Status setPointer(Context &ctx, const ResolvedPath &rp, const PropValue &value)
{
  const ApiMember &m = *rp.member;
  PointerRef target;
  if (!value.isNone()) {
    const StableRef *ref = value.tryPointer();
    if (!ref) {
      return err(StatusCode::TypeMismatch,
                 std::string(m.name()) + ": expected a Pointer, got " +
                     propValueTypeName(value.type()));
    }
    if (!ref->isNull()) {
      Result<ResolvedPath> t = resolve(ctx, *ref);
      if (!t.ok()) {
        return t.status();
      }
      if (t->kind != ResolvedPath::Kind::Struct) {
        return err(StatusCode::TypeMismatch, std::string(m.name()) + ": the value isn't a struct");
      }
      target = t->ptr;
    }
  }
  if (!target.isNull()) {
    const PointerAccessor &pa = m.pointer();
    const ApiRegistry &reg = ctx.runtime().ext<ApiRegistry>();
    bool ok = pa.target && reg.isA(target.type, pa.target);
    for (StructId c : pa.candidates) {
      ok = ok || reg.isA(target.type, c);
    }
    if (!ok) {
      return err(StatusCode::TypeMismatch,
                 std::string(m.name()) + ": the target is of the wrong struct type");
    }
  }
  CDCC_TRY(m.pointer().set(rp.ptr, ctx, target));
  if (ctx.library()) {
    ctx.library()->noteRefsChanged();
  }
  return Status();
}

/* The whole member's new value for a Component or EnumKey write. */
Result<PropValue> widen(Context &ctx, const ResolvedPath &rp, const PropValue &value)
{
  const ApiMember &m = *rp.member;
  Result<PropValue> read = readMember(ctx, rp);
  if (!read.ok()) {
    return read.status();
  }
  const PropValue &whole = *read;
  if (rp.kind == ResolvedPath::Kind::Component) {
    std::optional<double> x = value.tryFloat();
    if (!x) {
      if (std::optional<int64_t> i = value.tryInt()) {
        x = double(*i);
      }
    }
    if (!x) {
      return err(StatusCode::TypeMismatch,
                 std::string(m.name()) + "[" + std::to_string(rp.component) +
                     "]: expected a number");
    }
    double c[16];
    for (int i = 0; i < whole.componentCount(); i++) {
      c[i] = whole.component(i);
    }
    c[rp.component] = *x;
    return PropValue::ofComponents(whole.type(), c);
  }
  Result<PropValue> on = coerce(PropertyDef::boolProp("key"), value);
  if (!on.ok()) {
    return on.status();
  }
  const EnumItem *item = findItem(m, rp.enumKey.c_str());
  if (whole.type() == PropValue::Type::Flags) {
    const uint64_t bit = uint64_t(item->value);
    return PropValue::ofFlags(on->asBool() ? (whole.asFlags() | bit) : (whole.asFlags() & ~bit));
  }
  if (!on->asBool()) {
    return err(StatusCode::InvalidArgument,
               std::string(m.name()) + "[" + rp.enumKey.c_str() +
                   "]: an enum key can only be set, not cleared");
  }
  return PropValue::ofEnum(item->value);
}

} // namespace

Status setValue(Context &ctx, const ResolvedPath &rp, const PropValue &value)
{
  CDCC_TRY(ctx.runtime().busyIfEvaluating("setValue"));
  if (rp.kind == ResolvedPath::Kind::Struct || rp.kind == ResolvedPath::Kind::Length) {
    return err(StatusCode::FailedPrecondition, "setValue: the path is read-only");
  }
  const ApiMember &m = *rp.member;
  /* Read-only by flag, for lack of a setter, or per instance by a dynamic hook. */
  const PropertyDef def = effectiveDef(ctx, rp);
  if (m.readOnly() || def.hasAccess(PropAccess::ReadOnly)) {
    return err(StatusCode::FailedPrecondition,
               std::string("setValue: '") + m.name() + "' is read-only");
  }
  if (m.kind() == MemberKind::Pointer) {
    CDCC_TRY(setPointer(ctx, rp, value));
  }
  else {
    PropValue input = value;
    if (rp.kind != ResolvedPath::Kind::Member) {
      Result<PropValue> whole = widen(ctx, rp, value);
      if (!whole.ok()) {
        return whole.status();
      }
      input = std::move(whole).value();
    }
    Result<PropValue> v = coerce(def, input);
    if (!v.ok()) {
      return v.status();
    }
    CDCC_TRY(m.value().set(rp.ptr, ctx, *v));
  }
  ctx.runtime().ext<ChangeBus>().notify(rp.canonical.owner, m.name());
  if (m.updateFn()) {
    m.updateFn()(rp.ptr, ctx);
  }
  return Status();
}

Status setValue(Context &ctx, std::string_view path, const PropValue &value)
{
  Result<ResolvedPath> rp = resolveForWrite(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  return setValue(ctx, *rp, value);
}

void getValues(Context &ctx,
               std::span<const std::string_view> paths,
               Vector<PropValue> &values,
               Vector<Status> &errors)
{
  values.clear();
  errors.clear();
  for (std::string_view p : paths) {
    Result<PropValue> v = getValue(ctx, p);
    values.append(v.ok() ? *v : PropValue());
    errors.append(v.ok() ? Status() : v.status());
  }
}

} // namespace coredcc

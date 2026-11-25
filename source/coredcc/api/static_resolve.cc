#include "coredcc/api/static_resolve.h"

#include "coredcc/base/text.h"

#include <cstring>
#include <string>
#include <utility>

namespace coredcc {

using litestl::util::Vector;

namespace {

Status err(StatusCode code, const std::string &what)
{
  return Status::error(code, what.c_str());
}

void addAll(StaticStructs &to, const StaticStructs &more)
{
  to.unknown |= more.unknown;
  for (StructId s : more.structs) {
    if (!to.structs.contains(s)) {
      to.structs.append(s);
    }
  }
}

} // namespace

/* One resolution. */
struct StaticResolver::Walk {
  StaticResolver &r;
  const Path &path;
  bool through;
  /** What the path up to a segment can name. Members can take one segment or two, so the
   * structs of one step can be at different places. */
  Vector<StaticStructs> at;
  /** The types whose subtypes `at` has: many members can have one target. */
  Vector<Vector<StructId>> families;
  StaticResolution out;
  /** The error that came furthest, and the structs that had no such member there. */
  int errorAt = -1;
  Status error;
  Vector<StructId> without;

  Walk(StaticResolver &r, const StaticStructs &from, const Path &path, bool through)
      : r(r), path(path), through(through)
  {
    at.resize(path.size() + 1);
    families.resize(path.size() + 1);
    at[0] = from;
  }

  void fail(int segment, Status s)
  {
    if (segment > errorAt) {
      errorAt = segment;
      error = std::move(s);
      without.clear();
    }
  }
  void noMember(int segment, StructId type)
  {
    if (segment > errorAt) {
      fail(segment, Status()); /* failure() makes it: suggestions take time */
      without.append(type);
    }
    else if (segment == errorAt && without.size() > 0) {
      without.append(type);
    }
  }
  void family(int place, StructId type)
  {
    if (!families[place].contains(type)) {
      families[place].append(type);
      addAll(at[place], r.withSubtypes(type));
    }
  }
  void target(ResolvedPath::Kind kind, StructId type, const ApiMember &m)
  {
    StaticTarget t;
    t.kind = kind;
    t.type = type;
    t.member = &m;
    out.targets.append(t);
  }

  void run()
  {
    const int n = path.size();
    for (int i = 0; i < n; i++) {
      out.unknown |= at[i].unknown;
      if (at[i].structs.size() > 0) {
        step(i);
      }
    }
    out.unknown |= at[n].unknown;
    for (StructId s : at[n].structs) {
      StaticTarget t;
      t.type = s;
      out.targets.append(t);
    }
  }

  void step(int i)
  {
    const PathSegment &seg = path[i];
    if (seg.kind != SegmentKind::Member) {
      fail(i, err(StatusCode::InvalidArgument, "a subscript needs a collection or vector"));
      return;
    }
    Vector<const ApiMember *> seen; /* structs of one parent share its members */
    for (StructId type : at[i].structs) {
      const ApiMember *m = r.reg_.findMember(type, seg.text.c_str());
      if (!m) {
        noMember(i, type);
      }
      else if (!seen.contains(m)) {
        seen.append(m);
        member(i, type, *m);
      }
    }
  }

  void member(int i, StructId type, const ApiMember &m)
  {
    const bool last = i + 1 == path.size();
    switch (m.kind()) {
      case MemberKind::Value:
        if (last) {
          target(ResolvedPath::Kind::Member, type, m);
        }
        else {
          part(i, type, m);
        }
        break;
      case MemberKind::Pointer:
        if (last && !through) {
          target(ResolvedPath::Kind::Member, type, m);
        }
        else {
          pointer(i + 1, m.pointer());
        }
        break;
      case MemberKind::Collection:
        if (last) {
          target(ResolvedPath::Kind::Member, type, m);
        }
        else {
          item(i, type, m);
        }
        break;
    }
  }

  void pointer(int place, const PointerAccessor &p)
  {
    if (p.target) {
      family(place, p.target);
      return;
    }
    for (StructId c : p.candidates) {
      family(place, c);
    }
  }

  /* A component or an enum key. */
  void part(int i, StructId type, const ApiMember &m)
  {
    const PathSegment &sub = path[i + 1];
    const std::string name = std::string("'") + m.name() + "'";
    const PropKind kind = m.def().kind();
    if (i + 2 != path.size()) {
      fail(i + 2, err(StatusCode::InvalidArgument,
                      "nothing can follow '" + std::string(m.name()) + "[...]'"));
    }
    else if (sub.kind == SegmentKind::Index && componentCount(kind) > 1) {
      if (sub.index >= componentCount(kind)) {
        fail(i + 1, err(StatusCode::OutOfRange,
                        name + " has " + std::to_string(componentCount(kind)) + " components"));
        return;
      }
      target(ResolvedPath::Kind::Component, type, m);
    }
    else if (sub.kind == SegmentKind::EnumKey &&
             (kind == PropKind::Enum || kind == PropKind::Flags))
    {
      const EnumMeta *items = m.def().enumMeta();
      if (!items || !items->findKey(sub.text.c_str())) {
        fail(i + 1, err(StatusCode::NotFound, name + " has no key '" + sub.text.c_str() + "'"));
        return;
      }
      target(ResolvedPath::Kind::EnumKey, type, m);
    }
    else {
      fail(i + 1, err(StatusCode::InvalidArgument, name + " (" + propKindName(kind) +
                                                       ") has no members or components"));
    }
  }

  /* Something in a collection. */
  void item(int i, StructId type, const ApiMember &m)
  {
    const PathSegment &sub = path[i + 1];
    const CollectionAccessor &c = m.collection();
    const std::string name = std::string("'") + m.name() + "'";
    if (sub.kind == SegmentKind::EnumKey) {
      fail(i + 1, err(StatusCode::InvalidArgument,
                      name + " takes [index], [\"key\"], [#handle], .active or .length"));
      return;
    }
    if (sub.kind == SegmentKind::Member) {
      if (strcmp(sub.text.c_str(), "length") == 0) {
        if (i + 2 != path.size()) {
          fail(i + 2, err(StatusCode::InvalidArgument, "nothing can follow '.length'"));
          return;
        }
        target(ResolvedPath::Kind::Length, type, m);
        return;
      }
      if (strcmp(sub.text.c_str(), "active") != 0) {
        Status s = err(StatusCode::NotFound, "collection " + name + " has no member '" +
                                                 sub.text.c_str() +
                                                 "' (only .active and .length)");
        Suggester suggest(sub.text.c_str());
        suggest.consider("active");
        suggest.consider("length");
        suggest.addTo(s);
        fail(i + 1, std::move(s));
        return;
      }
      if (!c.active) {
        fail(i + 1, err(StatusCode::Unsupported, name + " has no active item"));
        return;
      }
    }
    if (c.staticItem && (sub.kind == SegmentKind::StringKey || sub.kind == SegmentKind::HandleKey)) {
      Result<StructId> st = c.staticItem(sub.kind == SegmentKind::StringKey ?
                                             KeySegment::stringKey(sub.text.c_str()) :
                                             KeySegment::ofHandle(sub.index));
      if (!st.ok()) {
        fail(i + 1, st.status());
        return;
      }
      addAll(at[i + 2], r.only(*st));
      return;
    }
    family(i + 2, c.itemStruct);
  }

  Status failure()
  {
    const char *name = without.size() > 0 ? path[errorAt].text.c_str() : "";
    if (without.size() == 1) {
      error = r.reg_.unknownMember(without[0], name);
    }
    else if (without.size() > 1) {
      std::string structs;
      Suggester suggest(name);
      for (int j = 0; j < int(without.size()); j++) {
        const ApiStruct *s = r.reg_.find(without[j]);
        if (j < 4) {
          structs += (j ? ", " : "") + std::string(s ? s->name().c_str() : "?");
        }
        for (int depth = 0; depth < 64 && s; depth++) {
          for (int k = 0; k < s->memberCount(); k++) {
            suggest.consider(s->member(k).name());
          }
          s = r.reg_.find(s->parentId());
        }
      }
      if (without.size() > 4) {
        structs += " and " + std::to_string(without.size() - 4) + " more";
      }
      error = err(StatusCode::NotFound,
                  "no member '" + std::string(name) + "' in any of " + structs);
      suggest.addTo(error);
    }
    if (errorAt < 0) {
      return err(StatusCode::NotFound, "there is no struct to start from");
    }
    return detail::atSegment(path[errorAt], error);
  }
};

StaticStructs StaticResolver::only(StructId type) const
{
  StaticStructs out;
  out.unknown = !reg_.find(type);
  if (!out.unknown) {
    out.structs.append(type);
  }
  return out;
}

const StaticStructs &StaticResolver::withSubtypes(StructId type)
{
  if (known_ != reg_.structCount()) {
    subtypes_.clear();
    known_ = reg_.structCount();
  }
  if (const auto found = subtypes_.find(type.value); found != subtypes_.end()) {
    return found->second;
  }
  StaticStructs out = only(type);
  if (!out.unknown) {
    for (int i = 0; i < reg_.structCount(); i++) {
      const StructId id = reg_.structAt(i).id();
      if (id != type && reg_.isA(id, type)) {
        out.structs.append(id);
      }
    }
  }
  return subtypes_[type.value] = std::move(out);
}

Result<StaticResolution> StaticResolver::resolve(const StaticStructs &from, const Path &path)
{
  Walk w(*this, from, path, false);
  w.run();
  if (w.out.targets.size() == 0 && !w.out.unknown) {
    return w.failure();
  }
  return std::move(w.out);
}

Result<StaticResolution> StaticResolver::resolve(StructId from, std::string_view path)
{
  Result<Path> p = parsePath(path);
  if (!p.ok()) {
    return p.status();
  }
  return resolve(only(from), *p);
}

Result<StaticStructs> StaticResolver::resolveStructs(const StaticStructs &from, const Path &path)
{
  Walk w(*this, from, path, true);
  w.run();
  StaticStructs out;
  out.unknown = w.out.unknown;
  for (const StaticTarget &t : w.out.targets) {
    if (t.kind == ResolvedPath::Kind::Struct) {
      out.structs.append(t.type);
    }
  }
  if (out.structs.size() == 0 && !out.unknown) {
    return w.out.targets.size() > 0 ?
               err(StatusCode::InvalidArgument, "'" + path.toString() + "' isn't a struct") :
               w.failure();
  }
  return out;
}

} // namespace coredcc

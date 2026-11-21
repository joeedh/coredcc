#include "coredcc/command/last_used.h"

#include "coredcc/base/text.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/literal.h"
#include "coredcc/parse/invocation.h"

#include <cstring>

namespace coredcc {

namespace {

bool storable(const PropertyDef &def)
{
  return def.kind() != PropKind::Pointer && def.kind() != PropKind::Collection;
}

constexpr const char *kItemBase = "CommandDefaults";

} // namespace

LastUsedStore::~LastUsedStore()
{
  clear();
}

const PropValue *LastUsedStore::Entry::find(std::string_view name) const
{
  for (const Value &v : values) {
    if (name == v.name.c_str()) {
      return &v.value;
    }
  }
  return nullptr;
}

LastUsedStore::Entry &LastUsedStore::entry(const CommandDef &def)
{
  if (Entry **e = byId_.lookup_ptr(def.id().value)) {
    return **e;
  }
  Entry *e = litestl::alloc::New<Entry>("coredcc::LastUsedStore::Entry");
  e->def = &def;
  entries_.append(e);
  byId_.add(def.id().value, e);
  return *e;
}

const LastUsedStore::Entry *LastUsedStore::findEntry(const CommandDef &def) const
{
  Entry *const *e = byId_.lookup_ptr(def.id().value);
  return e ? *e : nullptr;
}

void LastUsedStore::remember(const CommandDef &def, const PropertyBag &inputs)
{
  for (int i = 0; i < inputs.size(); i++) {
    if (inputs.wasSet(i) && storable(inputs.def(i))) {
      set(def, inputs.def(i).apiName().c_str(), inputs.value(i)).ignore(); /* coerced */
    }
  }
}

void LastUsedStore::apply(const CommandDef &def, PropertyBag &bag) const
{
  const Entry *e = findEntry(def);
  if (!e) {
    return;
  }
  for (const Value &v : e->values) {
    if (bag.indexOf(v.name.c_str()) >= 0) {
      bag.set(v.name.c_str(), v.value).ignore(); /* a def that changed: keep the default */
    }
  }
}

const PropValue *LastUsedStore::get(const CommandDef &def, std::string_view input) const
{
  const Entry *e = findEntry(def);
  return e ? e->find(input) : nullptr;
}

Status LastUsedStore::set(const CommandDef &def, std::string_view input, const PropValue &value)
{
  const PropertyDef *in = def.findInput(input);
  if (!in) {
    return Status::error(StatusCode::NotFound, (std::string(def.name().c_str()) +
                                                " has no input '" + std::string(input) + "'")
                                                   .c_str());
  }
  if (!storable(*in)) {
    return Status::error(StatusCode::Unsupported,
                         (std::string(input) + ": Pointer and Collection inputs aren't kept")
                             .c_str());
  }
  Result<PropValue> v = coerce(*in, value);
  if (!v.ok()) {
    return v.status();
  }
  Entry &e = entry(def);
  for (Value &old : e.values) {
    if (input == old.name.c_str()) {
      old.value = *v;
      return Status();
    }
  }
  e.values.append(Value{lsString(input), *v});
  return Status();
}

void LastUsedStore::forget(const CommandDef &def)
{
  if (Entry **e = byId_.lookup_ptr(def.id().value)) {
    (*e)->values.clear();
  }
}

void LastUsedStore::clear()
{
  for (Entry *e : entries_) {
    litestl::alloc::Delete(e);
  }
  entries_.clear();
  byId_.clear();
}

std::string LastUsedStore::serialize() const
{
  std::string out;
  for (const Entry *e : entries_) {
    if (e->values.size() == 0) {
      continue;
    }
    out += e->def->name().c_str();
    out += '(';
    bool first = true;
    for (const Value &v : e->values) {
      const PropertyDef *in = e->def->findInput(v.name.c_str());
      Result<std::string> lit = in ? formatLiteral(*in, v.value) : Result<std::string>("");
      if (!in || !lit.ok()) {
        continue;
      }
      out += first ? "" : " ";
      out += v.name.c_str();
      out += '=';
      out += *lit;
      first = false;
    }
    out += ")\n";
  }
  return out;
}

Status LastUsedStore::load(std::string_view text)
{
  Status first; /* the first bad line; the rest still load */
  int line = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view ln = text.substr(pos, end - pos);
    pos = end + 1;
    line++;
    while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ' || ln.back() == '\t')) {
      ln.remove_suffix(1);
    }
    if (ln.empty() || ln[0] == '#') {
      continue;
    }
    Result<Invocation> inv = parseInvocation(ln);
    if (!inv.ok()) {
      if (first.ok()) {
        first = Status::error(StatusCode::InvalidArgument,
                              ("LastUsed line " + std::to_string(line) + ": " +
                               inv.status().message())
                                  .c_str());
      }
      continue;
    }
    const CommandDef *def = rt_.ext<CommandRegistry>().find(inv->command.c_str());
    if (!def) {
      continue; /* a command that's gone */
    }
    for (const InvocationArg &arg : inv->args) {
      const PropertyDef *in = def->findInput(arg.name.c_str());
      if (!in) {
        continue;
      }
      Result<PropValue> v = parseLiteral(*in, arg.value);
      if (v.ok()) {
        set(*def, arg.name.c_str(), *v).ignore();
      }
    }
  }
  return first;
}

/* ---- datapath ---- */

StructId LastUsedStore::itemStruct(const CommandDef &def)
{
  ApiRegistry &reg = rt_.ext<ApiRegistry>();
  const std::string name = std::string(kItemBase) + ":" + def.name().c_str();
  const StructId id = StructId::of(name);
  if (reg.find(id)) {
    return id;
  }
  ApiStruct st(name);
  st.parent(StructId::of(kItemBase));
  for (int i = 0; i < def.inputCount(); i++) {
    const PropertyDef &in = def.inputDef(i);
    if (!storable(in)) {
      continue;
    }
    const std::string input = in.apiName().c_str();
    ValueAccessor a;
    a.get = [input](const PointerRef &self, Context &) {
      const Entry *e = static_cast<const Entry *>(self.data);
      const PropValue *v = e->find(input);
      return v ? *v : e->def->findInput(input)->defaultValue();
    };
    a.set = [this, input](const PointerRef &self, Context &, const PropValue &v) {
      return set(*static_cast<const Entry *>(self.data)->def, input, v);
    };
    PropertyDef d = in;
    d.access(PropAccess::NoUndo); /* preferences, not document edits */
    st.add(std::move(d), std::move(a));
  }
  Result<const ApiStruct *> added = reg.add(std::move(st));
  CDCC_CHECK(added.ok(), "LastUsedStore: registering a commandDefaults struct failed");
  return id;
}

void LastUsedStore::addCollectionTo(ApiStruct &root, std::string_view memberName)
{
  ApiRegistry &reg = rt_.ext<ApiRegistry>();
  if (!reg.find(StructId::of(kItemBase))) {
    CDCC_CHECK(reg.add(ApiStruct(kItemBase)).ok(), "LastUsedStore: base struct");
  }
  CollectionAccessor c;
  c.length = [this](const PointerRef &, Context &) {
    return int64_t(rt_.ext<CommandRegistry>().size());
  };
  c.at = [this](const PointerRef &, Context &, int64_t i) {
    const CommandDef &def = rt_.ext<CommandRegistry>().at(int(i));
    return PointerRef{&entry(def), itemStruct(def), OwnerKey()};
  };
  c.lookup = [this](const PointerRef &, Context &, const KeySegment &key) {
    const CommandDef *def = key.kind == KeySegment::Kind::StringKey ?
                                rt_.ext<CommandRegistry>().find(key.text.c_str()) :
                                nullptr;
    return def ? PointerRef{&entry(*def), itemStruct(*def), OwnerKey()} : PointerRef();
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &item) {
    return KeySegment::stringKey(static_cast<const Entry *>(item.data)->def->name().c_str());
  };
  root.addCollection(PropertyDef::collectionProp(memberName, StructId::of(kItemBase))
                         .access(PropAccess::NoUndo),
                     std::move(c));
}

Status LastUsedStore::validate() const
{
  if (int(byId_.size()) != int(entries_.size())) {
    return Status::error(StatusCode::Internal, "LastUsedStore: index and list differ");
  }
  return Status();
}

} // namespace coredcc

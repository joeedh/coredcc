#include "coredcc/command/last_used.h"

#include "coredcc/api/change_bus.h"
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
  destroyEntries();
}

LastUsedStore::Entry &LastUsedStore::entry(const CommandDef &def)
{
  if (Entry **e = byId_.lookup_ptr(def.id().value)) {
    return **e;
  }
  Entry *e = litestl::alloc::New<Entry>("coredcc::LastUsedStore::Entry");
  static_cast<PropertyBag &>(*e) = def.newInputs();
  e->command = &def;
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
  for (int i : inputs.setOrder()) {
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
  for (int i : e->setOrder()) {
    const char *name = e->def(i).apiName().c_str();
    if (e->wasSet(i) && bag.indexOf(name) >= 0) {
      bag.set(name, e->value(i)).ignore(); /* a def that changed: keep the default */
    }
  }
}

const PropValue *LastUsedStore::get(const CommandDef &def, std::string_view input) const
{
  const Entry *e = findEntry(def);
  return e && e->isSet(input) ? e->PropertyBag::get(input) : nullptr;
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
  Entry &e = entry(def);
  const int i = e.indexOf(input);
  const PropValue before = e.value(i);
  const bool wasSet = e.wasSet(i);
  CDCC_TRY(e.setAt(i, value));
  if (e.wasSet(i) != wasSet || !(e.value(i) == before)) {
    changed();
  }
  return Status();
}

void LastUsedStore::forget(const CommandDef &def)
{
  if (Entry **e = byId_.lookup_ptr(def.id().value)) {
    (*e)->resetAll();
    changed();
  }
}

void LastUsedStore::clear()
{
  destroyEntries();
  changed();
}

void LastUsedStore::changed()
{
  /* Not a notify: the owner of the values is whatever root has the collection. */
  rt_.ext<ChangeBus>().bumpEpoch();
}

void LastUsedStore::destroyEntries()
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
    std::string args;
    for (int i = 0; i < e->size(); i++) {
      if (!e->wasSet(i)) {
        continue;
      }
      Result<std::string> lit = formatLiteral(rt_, e->resolvedDef(i), e->value(i));
      if (lit.ok()) {
        args += (args.empty() ? "" : " ") + std::string(e->def(i).apiName().c_str()) + "=" + *lit;
      }
    }
    if (!args.empty()) {
      out += std::string(e->command->name().c_str()) + "(" + args + ")\n";
    }
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
    const Entry &e = entry(*def);
    for (bool dynamic : {false, true}) {
      for (const InvocationArg &arg : inv->args) {
        const int i = e.indexOf(arg.name.c_str());
        if (i < 0 || e.def(i).isDynamic() != dynamic) {
          continue;
        }
        Result<PropValue> v = parseLiteral(rt_, e.resolvedDef(i), arg.value);
        if (v.ok()) {
          set(*def, arg.name.c_str(), *v).ignore();
        }
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
      const PropertyBag &bag = *static_cast<const PropertyBag *>(self.data);
      const int i = bag.indexOf(input);
      return bag.wasSet(i) ? bag.value(i) : bag.def(i).defaultValue();
    };
    const CommandDef *command = &def; /* registered, so it stays */
    a.set = [this, command, input](const PointerRef &, Context &, const PropValue &v) {
      return set(*command, input, v);
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
    return PointerRef{static_cast<PropertyBag *>(&entry(def)), itemStruct(def), OwnerKey()};
  };
  c.lookup = [this](const PointerRef &, Context &, const KeySegment &key) {
    const CommandDef *def = key.kind == KeySegment::Kind::StringKey ?
                                rt_.ext<CommandRegistry>().find(key.text.c_str()) :
                                nullptr;
    PropertyBag *bag = def ? &entry(*def) : nullptr;
    return bag ? PointerRef{bag, itemStruct(*def), OwnerKey()} : PointerRef();
  };
  c.staticItem = [this](const KeySegment &key) -> Result<StructId> {
    const CommandRegistry &commands = rt_.ext<CommandRegistry>();
    const CommandDef *def =
        key.kind == KeySegment::Kind::StringKey ? commands.find(key.text.c_str()) : nullptr;
    return def ? Result<StructId>(itemStruct(*def)) : commands.unknown(key.text.c_str());
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &item) {
    const auto *e = static_cast<const Entry *>(static_cast<const PropertyBag *>(item.data));
    return KeySegment::stringKey(e->command->name().c_str());
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

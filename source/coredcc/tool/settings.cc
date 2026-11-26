#include "coredcc/tool/settings.h"

#include "coredcc/api/change_bus.h"
#include "coredcc/command/literal.h"
#include "coredcc/parse/invocation.h"

namespace coredcc {

namespace {

constexpr const char *kItemBase = "coredcc.ToolSettings";

} // namespace

ToolSettingsStore::ToolSettingsStore(Runtime *rt) : rt_(*rt)
{
  ApiRegistry &reg = rt_.ext<ApiRegistry>();
  if (!reg.find(baseStruct())) {
    CDCC_CHECK(reg.add(ApiStruct(kItemBase)).ok(), "ToolSettingsStore: base struct");
  }
}

ToolSettingsStore::~ToolSettingsStore()
{
  for (Entry *e : entries_) {
    litestl::alloc::Delete(e);
  }
}

ToolSettingsStore::Entry &ToolSettingsStore::entry(const ToolDef &def)
{
  if (Entry **e = byKey_.lookup_ptr(def.key())) {
    return **e;
  }
  Entry *e = litestl::alloc::New<Entry>("coredcc::ToolSettingsStore::Entry");
  for (int i = 0; i < def.settingCount(); i++) {
    e->add(def.settingDef(i)).ignore(); /* checked at registration */
  }
  e->tool = &def;
  entries_.append(e);
  byKey_.add(def.key(), e);
  return *e;
}

const ToolSettingsStore::Entry *ToolSettingsStore::findEntry(const ToolDef &def) const
{
  Entry *const *e = byKey_.lookup_ptr(def.key());
  return e ? *e : nullptr;
}

const PropertyBag &ToolSettingsStore::settings(const ToolDef &def)
{
  return entry(def);
}

const PropValue *ToolSettingsStore::get(const ToolDef &def, std::string_view name)
{
  Entry &e = entry(def);
  const int i = e.indexOf(name);
  return i >= 0 ? &e.value(i) : nullptr;
}

Status
ToolSettingsStore::set(const ToolDef &def, std::string_view name, const PropValue &value)
{
  Entry &e = entry(def);
  const int i = e.indexOf(name);
  if (i < 0) {
    return Status::error(
        StatusCode::NotFound,
        (std::string(def.name().c_str()) + " has no setting '" + std::string(name) + "'")
            .c_str());
  }
  const PropValue before = e.value(i);
  CDCC_TRY(e.setAt(i, value));
  if (!(e.value(i) == before)) {
    e.version++;
    rt_.ext<ChangeBus>().bumpEpoch();
  }
  return Status();
}

void ToolSettingsStore::reset(const ToolDef &def)
{
  Entry &e = entry(def);
  e.resetAll();
  e.version++;
  rt_.ext<ChangeBus>().bumpEpoch();
}

uint64_t ToolSettingsStore::version(const ToolDef &def) const
{
  const Entry *e = findEntry(def);
  return e ? e->version : 1;
}

std::string ToolSettingsStore::serialize() const
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
        args += (args.empty() ? "" : " ") + std::string(e->def(i).apiName().c_str()) +
                "=" + *lit;
      }
    }
    if (!args.empty()) {
      out += std::string(e->tool->name().c_str()) + "(" + args + ")\n";
    }
  }
  return out;
}

Status ToolSettingsStore::load(std::string_view text)
{
  Status first;
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
        first = Status::error(
            StatusCode::InvalidArgument,
            ("tool settings line " + std::to_string(line) + ": " + inv.status().message())
                .c_str());
      }
      continue;
    }
    const ToolDef *def = rt_.ext<ToolRegistry>().find(inv->command.c_str());
    if (!def) {
      continue; /* a tool that's gone */
    }
    const Entry &e = entry(*def);
    for (const InvocationArg &arg : inv->args) {
      const int i = e.indexOf(arg.name.c_str());
      if (i < 0) {
        continue;
      }
      Result<PropValue> v = parseLiteral(rt_, e.resolvedDef(i), arg.value);
      if (v.ok()) {
        set(*def, arg.name.c_str(), *v).ignore();
      }
    }
  }
  return first;
}

/* ---- datapath ---- */

StructId ToolSettingsStore::baseStruct()
{
  return StructId::of(kItemBase);
}

StructId ToolSettingsStore::itemStruct(const ToolDef &def)
{
  ApiRegistry &reg = rt_.ext<ApiRegistry>();
  const std::string name = std::string(kItemBase) + ":" + def.name().c_str();
  const StructId id = StructId::of(name);
  if (reg.find(id)) {
    return id;
  }
  ApiStruct st(name);
  st.parent(baseStruct());
  for (int i = 0; i < def.settingCount(); i++) {
    const std::string setting = def.settingDef(i).apiName().c_str();
    ValueAccessor a;
    a.get = [setting](const PointerRef &self, Context &) {
      const PropertyBag &bag = *static_cast<const PropertyBag *>(self.data);
      return bag.value(bag.indexOf(setting));
    };
    const ToolDef *tool = &def; /* registered, so it stays */
    a.set = [this, tool, setting](const PointerRef &, Context &, const PropValue &v) {
      return set(*tool, setting, v);
    };
    PropertyDef d = def.settingDef(i);
    d.access(PropAccess::NoUndo);
    st.add(std::move(d), std::move(a));
  }
  Result<const ApiStruct *> added = reg.add(std::move(st));
  CDCC_CHECK(added.ok(), "ToolSettingsStore: registering a toolSettings struct failed");
  return id;
}

void ToolSettingsStore::addCollectionTo(ApiStruct &root, std::string_view memberName)
{
  CollectionAccessor c;
  c.length = [this](const PointerRef &, Context &) {
    return int64_t(rt_.ext<ToolRegistry>().size());
  };
  c.at = [this](const PointerRef &, Context &, int64_t i) {
    const ToolDef &def = rt_.ext<ToolRegistry>().at(int(i));
    return PointerRef{
        static_cast<PropertyBag *>(&entry(def)), itemStruct(def), OwnerKey()};
  };
  c.lookup = [this](const PointerRef &, Context &, const KeySegment &key) {
    const ToolDef *def = key.kind == KeySegment::Kind::StringKey
                             ? rt_.ext<ToolRegistry>().find(key.text.c_str())
                             : nullptr;
    return def ? PointerRef{static_cast<PropertyBag *>(&entry(*def)),
                            itemStruct(*def),
                            OwnerKey()}
               : PointerRef();
  };
  c.staticItem = [this](const KeySegment &key) -> Result<StructId> {
    const ToolRegistry &tools = rt_.ext<ToolRegistry>();
    const ToolDef *def =
        key.kind == KeySegment::Kind::StringKey ? tools.find(key.text.c_str()) : nullptr;
    return def ? Result<StructId>(itemStruct(*def)) : tools.unknown(key.text.c_str());
  };
  c.keyOf = [](const PointerRef &, Context &, const PointerRef &item) {
    const auto *e =
        static_cast<const Entry *>(static_cast<const PropertyBag *>(item.data));
    return KeySegment::stringKey(e->tool->name().c_str());
  };
  root.addCollection(
      PropertyDef::collectionProp(memberName, baseStruct()).access(PropAccess::NoUndo),
      std::move(c));
}

} // namespace coredcc

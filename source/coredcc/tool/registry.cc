/* ToolDef and ToolRegistry. */
#include "coredcc/api/struct.h"
#include "coredcc/base/text.h"
#include "coredcc/layout/validate.h"
#include "coredcc/layout/xml.h"
#include "coredcc/props/bag.h"
#include "coredcc/tool/settings.h"
#include "coredcc/tool/tool.h"

#include <cstdio>
#include <string>

namespace coredcc {

/* ---- ToolDef ---- */

ToolDef::ToolDef(std::string_view id)
    : name_(lsString(id)), key_(TypeId::of(id).value),
      uiName_(lsString(uiNameFromApiName(id.substr(id.rfind('.') + 1))))
{
}

ToolDef &ToolDef::impl(std::string_view implName, ToolFactory factory)
{
  implName_ = lsString(implName);
  factory_ = std::move(factory);
  return *this;
}

ToolDef &ToolDef::uiName(std::string_view s)
{
  uiName_ = lsString(s);
  return *this;
}

ToolDef &ToolDef::description(std::string_view s)
{
  description_ = lsString(s);
  return *this;
}

ToolDef &ToolDef::icon(std::string_view s)
{
  icon_ = lsString(s);
  return *this;
}

ToolDef &ToolDef::setting(PropertyDef def)
{
  settings_.append(std::move(def));
  return *this;
}

ToolDef &ToolDef::layout(std::string_view xml)
{
  layout_ = lsString(xml);
  return *this;
}

const PropertyDef *ToolDef::findSetting(std::string_view name) const
{
  for (const PropertyDef &d : settings_) {
    if (name == d.apiName().c_str()) {
      return &d;
    }
  }
  return nullptr;
}

/* ---- ToolRegistry ---- */

ToolRegistry::~ToolRegistry()
{
  for (ToolDef *d : defs_) {
    litestl::alloc::Delete(d);
  }
}

namespace {

Status refuse(const std::string &tool, const std::string &why)
{
  return Status::error(StatusCode::InvalidArgument,
                       ("tool '" + tool + "': " + why).c_str());
}

/* A struct of the settings' defs alone, to check a layout against before the def is
 * registered. Named by the settings' names and kinds, so checking the same def again (as
 * addNativeTool and add both do) finds the one made before. Its accessors are never
 * called. */
StructId scratchStruct(Runtime &rt, const ToolDef &def)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  rt.ext<ToolSettingsStore>(); /* its base struct */
  std::string shape;
  for (int i = 0; i < def.settingCount(); i++) {
    const PropertyDef &s = def.settingDef(i);
    shape += std::string(s.apiName().c_str()) + ":" + std::to_string(int(s.kind())) + ";";
  }
  char hash[17];
  snprintf(hash, sizeof hash, "%016llx", (unsigned long long)TypeId::of(shape).value);
  const std::string name =
      std::string("coredcc.ToolSettingsCheck:") + def.name().c_str() + "#" + hash;
  if (reg.find(StructId::of(name))) {
    return StructId::of(name);
  }
  ApiStruct st(name);
  st.parent(ToolSettingsStore::baseStruct());
  for (int i = 0; i < def.settingCount(); i++) {
    ValueAccessor a;
    a.get = [](const PointerRef &, Context &) { return PropValue(); };
    st.add(def.settingDef(i), std::move(a));
  }
  return reg.add(std::move(st)).ok() ? StructId::of(name) : StructId();
}

} // namespace

Status ToolRegistry::checkDef(const ToolDef &def) const
{
  const std::string name = def.name().c_str();
  for (size_t start = 0;;) {
    const size_t dot = name.find('.', start);
    if (!isIdentifier(std::string_view(name).substr(start, dot - start))) {
      return refuse(name, "not a dotted identifier (like 'core.tool.select')");
    }
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  if (find(name)) {
    return Status::error(StatusCode::AlreadyExists,
                         ("tool '" + name + "' exists").c_str());
  }
  if (!def.hasImpl()) {
    return refuse(name, "no implementation");
  }
  PropertyBag bag;
  for (int i = 0; i < def.settingCount(); i++) {
    const PropertyDef &s = def.settingDef(i);
    if (s.kind() == PropKind::Pointer || s.kind() == PropKind::Collection) {
      return refuse(name,
                    std::string("setting '") + s.apiName().c_str() +
                        "': Pointer and Collection settings aren't kept");
    }
    if (Status st = bag.add(s); !st.ok()) {
      return refuse(name, std::string("setting: ") + st.message());
    }
  }
  if (def.layoutXml().size() > 0) {
    Result<Layout> layout = parseLayout(def.layoutXml().c_str());
    if (!layout.ok()) {
      return refuse(name, std::string("its layout: ") + layout.status().message());
    }
    const StructId root = scratchStruct(rt_, def);
    for (const LayoutIssue &issue : validateLayout(rt_, root, *layout)) {
      if (issue.severity == LayoutSeverity::Error) {
        return refuse(name, std::string("its layout: ") + issue.status.message());
      }
    }
  }
  return Status();
}

Result<const ToolDef *> ToolRegistry::add(ToolDef def)
{
  CDCC_TRY(rt_.busyIfEvaluating("ToolRegistry::add"));
  CDCC_TRY(checkDef(def));
  ToolDef *d = litestl::alloc::New<ToolDef>("coredcc::ToolDef", std::move(def));
  defs_.append(d);
  byId_.add(d->key(), d);
  return static_cast<const ToolDef *>(d);
}

const ToolDef *ToolRegistry::find(std::string_view id) const
{
  ToolDef *const *d = byId_.lookup_ptr(TypeId::of(id).value);
  return d && id == (*d)->name().c_str() ? *d : nullptr;
}

Status ToolRegistry::unknown(std::string_view id) const
{
  Status err =
      Status::error(StatusCode::NotFound, ("no tool '" + std::string(id) + "'").c_str());
  Suggester sg(id);
  for (const ToolDef *d : defs_) {
    sg.consider(d->name().c_str());
  }
  sg.addTo(err);
  return err;
}

} // namespace coredcc

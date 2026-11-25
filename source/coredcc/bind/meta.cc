#include "coredcc/bind/meta.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/props/units.h"

#include <cmath>

namespace coredcc {

namespace {

/* litestl strings in, a field out; nothing for an empty one. */
void text(JsonWriter &out, const char *key, const litestl::util::string &s)
{
  if (s.size() > 0) {
    out.key(key).string(s.c_str());
  }
}

void bound(JsonWriter &out, const char *key, double v)
{
  if (std::isfinite(v)) {
    out.key(key).number(v);
  }
}

void writeAccess(JsonWriter &out, PropAccess access)
{
  static constexpr struct {
    PropAccess flag;
    const char *name;
  } kNames[] = {
      {PropAccess::ReadOnly, "ReadOnly"},           {PropAccess::Private, "Private"},
      {PropAccess::NoUndo, "NoUndo"},               {PropAccess::Optional, "Optional"},
      {PropAccess::SaveLastValue, "SaveLastValue"}, {PropAccess::Animatable, "Animatable"},
  };
  out.key("access").beginArray();
  for (const auto &n : kNames) {
    if (hasFlag(access, n.flag)) {
      out.string(n.name);
    }
  }
  out.endArray();
}

void writeUi(JsonWriter &out, const PropUi &ui)
{
  out.key("ui").beginObject();
  if (ui.iconsOnly) {
    out.key("iconsOnly").boolean(true);
  }
  if (ui.expand) {
    out.key("expand").boolean(true);
  }
  if (ui.slider != SliderStyle::Default) {
    out.key("slider").string(ui.slider == SliderStyle::Slider ? "Slider" : "Number");
  }
  if (ui.multiline) {
    out.key("multiline").boolean(true);
  }
  out.endObject();
}

/* The keys of the items whose bits are all set; bits without an item as a number. */
void writeFlags(JsonWriter &out, const EnumMeta &meta, uint64_t bits)
{
  out.beginArray();
  for (const EnumItem &item : meta.items) {
    const uint64_t bit = uint64_t(item.value);
    if (bit != 0 && (bits & bit) == bit) {
      out.string(item.key.c_str());
      bits &= ~bit;
    }
  }
  if (bits != 0) {
    out.unsignedInteger(bits);
  }
  out.endArray();
}

void writeValue(JsonWriter &out, const Runtime &rt, const PropertyDef &def, const PropValue &v)
{
  switch (v.type()) {
    case PropValue::Type::None:
      out.null();
      return;
    case PropValue::Type::Bool:
      out.boolean(v.asBool());
      return;
    case PropValue::Type::Int:
      out.integer(v.asInt());
      return;
    case PropValue::Type::Float:
      out.number(v.asFloat());
      return;
    case PropValue::Type::String:
      out.string(v.asString().c_str());
      return;
    case PropValue::Type::Enum: {
      const EnumItem *item = def.enumMeta() ? def.enumMeta()->findValue(v.asEnum()) : nullptr;
      if (item) {
        out.string(item->key.c_str());
      }
      else {
        out.integer(v.asEnum());
      }
      return;
    }
    case PropValue::Type::Flags:
      writeFlags(out, def.enumMeta() ? *def.enumMeta() : EnumMeta(), v.asFlags());
      return;
    case PropValue::Type::Pointer:
      if (v.asPointer().isNull()) {
        out.null();
      }
      else {
        out.string(formatRef(rt, v.asPointer()));
      }
      return;
    default:
      out.beginArray();
      for (int i = 0; i < v.componentCount(); i++) {
        out.number(v.component(i));
      }
      out.endArray();
  }
}

void writeNumeric(JsonWriter &out, const NumericMeta &n)
{
  bound(out, "hardMin", n.hardMin);
  bound(out, "hardMax", n.hardMax);
  bound(out, "softMin", std::isfinite(n.softMin) ? n.softMin : n.hardMin);
  bound(out, "softMax", std::isfinite(n.softMax) ? n.softMax : n.hardMax);
  if (n.step != 0) {
    out.key("step").number(n.step);
  }
  out.key("precision").integer(n.precision);
  if (n.unit != UnitFamily::None) {
    out.key("unit").string(unitFamilyName(n.unit));
  }
}

void writeItems(JsonWriter &out, const EnumMeta &meta)
{
  out.key("items").beginArray();
  for (const EnumItem &item : meta.items) {
    out.beginObject();
    out.key("key").string(item.key.c_str());
    out.key("value").integer(item.value);
    out.key("uiName").string(item.uiName.c_str());
    text(out, "description", item.description);
    text(out, "icon", item.icon);
    out.endObject();
  }
  out.endArray();
}

void writeStructName(JsonWriter &out, const Runtime &rt, const char *key, StructId id)
{
  if (const char *name = id ? rt.nameOf(id) : nullptr) {
    out.key(key).string(name);
  }
}

void writeStruct(JsonWriter &out, const ApiRegistry &reg, StructId id)
{
  const ApiStruct *st = reg.find(id);
  if (st) {
    out.key("uiName").string(st->uiNameText().c_str());
    text(out, "description", st->descriptionText());
  }
  out.key("members").beginArray();
  /* Bounded, in case of a parent cycle. */
  for (int depth = 0; depth < 64 && st; depth++, st = reg.find(st->parentId())) {
    for (int i = 0; i < st->memberCount(); i++) {
      const ApiMember &m = st->member(i);
      const bool own = reg.findMember(id, m.name()) == &m; /* not hidden by a child's */
      if (own && !m.def().hasAccess(PropAccess::Private)) {
        out.string(m.name());
      }
    }
  }
  out.endArray();
}

} // namespace

const char *pathKindName(const ResolvedPath &rp)
{
  switch (rp.kind) {
    case ResolvedPath::Kind::Struct:
      return "Struct";
    case ResolvedPath::Kind::Member:
      return propKindName(rp.member->def().kind());
    case ResolvedPath::Kind::Component:
      return "Component";
    case ResolvedPath::Kind::EnumKey:
      return "EnumKey";
    case ResolvedPath::Kind::Length:
      return "Length";
  }
  return "";
}

void writePropertyMeta(JsonWriter &out, const Runtime &rt, const PropertyDef &def)
{
  out.key("name").string(def.apiName().c_str());
  out.key("uiName").string(def.uiName().c_str());
  text(out, "description", def.description());
  text(out, "icon", def.icon());
  if (def.subtype() != PropSubtype::None) {
    out.key("subtype").string(propSubtypeName(def.subtype()));
  }
  writeAccess(out, def.access());
  writeUi(out, def.ui());
  if (const NumericMeta *n = def.numeric()) {
    writeNumeric(out, *n);
  }
  if (const EnumMeta *e = def.enumMeta()) {
    writeItems(out, *e);
  }
  if (const StringMeta *s = def.stringMeta(); s && s->maxLength > 0) {
    out.key("maxLength").integer(s->maxLength);
  }
  if (const TargetMeta *t = def.target()) {
    writeStructName(out, rt, "target", t->target);
  }
  if (!def.defaultValue().isNone()) {
    out.key("default");
    writeValue(out, rt, def, def.defaultValue());
  }
}

Result<std::string> pathMeta(Context &ctx, std::string_view path)
{
  const Runtime &rt = ctx.runtime();
  const ApiRegistry &reg = ctx.runtime().ext<ApiRegistry>();
  Result<ResolvedPath> rp = resolve(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  JsonWriter out;
  out.beginObject();
  out.key("kind").string(pathKindName(*rp));
  out.key("canonical").string(formatRef(rt, rp->canonical));
  writeStructName(out, rt, "struct", rp->ptr.type);
  if (rp->kind == ResolvedPath::Kind::Struct) {
    writeStruct(out, reg, rp->ptr.type);
    out.endObject();
    return out.take();
  }
  Result<PropertyDef> def = getMeta(ctx, *rp);
  if (!def.ok()) {
    return def.status();
  }
  if (rp->kind == ResolvedPath::Kind::Component) {
    out.key("component").integer(rp->component);
    out.key("of").string(propKindName(def->kind()));
  }
  else if (rp->kind == ResolvedPath::Kind::EnumKey) {
    out.key("key").string(rp->enumKey.c_str());
    out.key("of").string(propKindName(rp->member->def().kind()));
    if (const EnumItem *item = rp->member->def().enumMeta()->findKey(rp->enumKey.c_str())) {
      def->uiName(item->uiName.c_str()).description(item->description.c_str());
      def->icon(item->icon.c_str());
    }
  }
  else if (rp->kind == ResolvedPath::Kind::Length) {
    writeStructName(out, rt, "target", rp->member->collection().itemStruct);
  }
  out.key("readOnly").boolean(def->hasAccess(PropAccess::ReadOnly));
  writePropertyMeta(out, rt, *def);
  out.endObject();
  return out.take();
}

} // namespace coredcc

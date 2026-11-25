#include "coredcc/api/format.h"

#include "coredcc/api/resolve.h"

#include <charconv>
#include <cmath>
#include <optional>

namespace coredcc {

namespace {

std::string_view trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

Status badText(const PropertyDef &def, const char *why, std::string_view text)
{
  return Status::error(StatusCode::InvalidArgument,
                       (std::string(def.apiName().c_str()) + ": " + why + ", got '" +
                        std::string(text) + "'")
                           .c_str());
}

/* The settings that a def's numbers are shown with. */
UnitSettings settingsFor(const PropertyDef &def, const UnitSettings &settings)
{
  UnitSettings out = settings;
  const NumericMeta *n = def.numeric();
  if (n && def.subtype() == PropSubtype::Percentage) {
    (void)out.setDisplayUnit(UnitFamily::Factor, "percent");
  }
  return out;
}

std::string itemName(const EnumMeta &meta, int64_t value)
{
  const EnumItem *item = meta.findValue(value);
  return item ? std::string(item->uiName.c_str()) : std::to_string(value);
}

/* By key, or by UI name. */
const EnumItem *findItem(const EnumMeta &meta, std::string_view text)
{
  if (const EnumItem *item = meta.findKey(text)) {
    return item;
  }
  for (const EnumItem &item : meta.items) {
    if (text == item.uiName.c_str()) {
      return &item;
    }
  }
  return nullptr;
}

Result<PropValue> parseEnum(const PropertyDef &def, std::string_view text)
{
  if (const EnumItem *item = findItem(*def.enumMeta(), text)) {
    return PropValue::ofEnum(item->value);
  }
  return coerce(def, PropValue::ofString(text)); /* its error, with suggestions */
}

Result<PropValue> parseFlags(const PropertyDef &def, std::string_view text)
{
  uint64_t bits = 0;
  while (!text.empty()) {
    const size_t end = text.find_first_of(",|");
    const std::string_view part = trim(text.substr(0, end));
    const EnumItem *item = findItem(*def.enumMeta(), part);
    if (!item) {
      return part.empty() ? Result<PropValue>(badText(def, "expected flags", text))
                          : coerce(def, PropValue::ofString(part));
    }
    bits |= uint64_t(item->value);
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (end != std::string_view::npos && trim(text).empty()) {
      return badText(def, "expected a flag after the last comma", text);
    }
  }
  return PropValue::ofFlags(bits);
}

Result<PropValue> parseInt(const PropertyDef &def,
                           std::string_view text,
                           UnitFamily family,
                           const UnitSettings &settings)
{
  if (family == UnitFamily::None) {
    const std::string_view digits =
        text.starts_with('+') && !text.starts_with("+-") ? text.substr(1) : text;
    int64_t v = 0;
    const char *end = digits.data() + digits.size();
    const std::from_chars_result r = std::from_chars(digits.data(), end, v);
    if (r.ec == std::errc() && r.ptr == end) {
      return coerce(def, PropValue::ofInt(v));
    }
  }
  Result<double> d = units::parse(text, family, settings);
  if (!d.ok()) {
    return d.status();
  }
  const double whole = std::round(*d);
  const bool near =
      family != UnitFamily::None && std::fabs(*d - whole) <= 1e-9 * std::fabs(whole);
  return coerce(def, PropValue::ofFloat(near ? whole : *d));
}

Result<PropValue> parseComponents(const PropertyDef &def,
                                  std::string_view text,
                                  UnitFamily family,
                                  const UnitSettings &settings)
{
  const int count = componentCount(def.kind());
  double values[16] = {};
  int n = 0;
  for (std::string_view rest = text;;) {
    const size_t comma = rest.find(',');
    Result<double> d = units::parse(rest.substr(0, comma), family, settings);
    if (!d.ok()) {
      return d.status();
    }
    if (n == 16) {
      return badText(def, "too many numbers", text);
    }
    values[n++] = *d;
    if (comma == std::string_view::npos) {
      break;
    }
    rest = rest.substr(comma + 1);
  }
  if (n == 1 && (count == 1 || isVector(def.kind()))) {
    return coerce(def, PropValue::ofFloat(values[0]));
  }
  if (n != count) {
    return badText(def, "wrong number of components", text);
  }
  return coerce(def, PropValue::ofComponents(valueTypeFor(def.kind()), values));
}

/* The def of one component of a vector, as a Float. */
PropertyDef componentDef(const PropertyDef &def)
{
  PropertyDef out = PropertyDef::floatProp(def.apiName().c_str());
  const NumericMeta &n = *def.numeric();
  out.subtype(def.subtype()).precision(n.precision).unit(n.unit);
  return out;
}

} // namespace

std::string
formatValue(const PropertyDef &def, const PropValue &value, const UnitSettings &settings)
{
  const NumericMeta *n = def.numeric();
  if (n && value.componentCount() > 0) {
    const UnitSettings s = settingsFor(def, settings);
    std::string out;
    for (int i = 0; i < value.componentCount(); i++) {
      out += i ? ", " : "";
      out += units::format(value.component(i), n->unit, s, n->precision);
    }
    return out;
  }
  if (const std::optional<int64_t> i = value.tryInt()) {
    return n && n->unit != UnitFamily::None
               ? units::format(
                     double(*i), n->unit, settingsFor(def, settings), n->precision)
               : std::to_string(*i);
  }
  if (const std::optional<bool> b = value.tryBool()) {
    return *b ? "true" : "false";
  }
  if (const std::optional<int64_t> e = value.tryEnum(); e && def.enumMeta()) {
    return itemName(*def.enumMeta(), *e);
  }
  if (const std::optional<uint64_t> f = value.tryFlags(); f && def.enumMeta()) {
    std::string out;
    for (int bit = 0; bit < 64; bit++) {
      if (*f >> bit & 1) {
        out += out.empty() ? "" : ", ";
        out += itemName(*def.enumMeta(), int64_t(uint64_t(1) << bit));
      }
    }
    return out;
  }
  if (const litestl::util::string *s = value.tryString()) {
    return s->c_str();
  }
  return value.toString();
}

Result<PropValue>
parseValue(const PropertyDef &def, std::string_view text, const UnitSettings &settings)
{
  const NumericMeta *n = def.numeric();
  const UnitFamily family = n ? n->unit : UnitFamily::None;
  const std::string_view s = trim(text);
  switch (def.kind()) {
  case PropKind::Bool:
    if (s == "true" || s == "on" || s == "yes" || s == "1") {
      return PropValue::ofBool(true);
    }
    if (s == "false" || s == "off" || s == "no" || s == "0") {
      return PropValue::ofBool(false);
    }
    return badText(def, "expected true or false", text);
  case PropKind::Int:
    return parseInt(def, s, family, settingsFor(def, settings));
  case PropKind::Float:
  case PropKind::Vec2:
  case PropKind::Vec3:
  case PropKind::Vec4:
  case PropKind::Quat:
  case PropKind::Mat4:
    return parseComponents(def, s, family, settingsFor(def, settings));
  case PropKind::String:
    return coerce(def, PropValue::ofString(text));
  case PropKind::Enum:
    return parseEnum(def, s);
  case PropKind::Flags:
    return parseFlags(def, s);
  case PropKind::Pointer:
  case PropKind::Collection:
    break;
  }
  return Status::error(StatusCode::Unsupported,
                       (std::string(def.apiName().c_str()) + ": a " +
                        propKindName(def.kind()) + " can't be read from text")
                           .c_str());
}

Result<std::string> formatValue(Context &ctx, std::string_view path)
{
  Result<ResolvedPath> rp = resolve(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  Result<PropertyDef> def = getMeta(ctx, *rp);
  if (!def.ok()) {
    return def.status();
  }
  Result<PropValue> value = getValue(ctx, *rp);
  if (!value.ok()) {
    return value.status();
  }
  return formatValue(*def, *value, ctx.units());
}

Result<PropValue> parseValue(Context &ctx, std::string_view path, std::string_view text)
{
  Result<ResolvedPath> rp = resolveForWrite(ctx, path);
  if (!rp.ok()) {
    return rp.status();
  }
  Result<PropertyDef> def = getMeta(ctx, *rp);
  if (!def.ok()) {
    return def.status();
  }
  if (rp->kind == ResolvedPath::Kind::Component) {
    return parseValue(componentDef(*def), text, ctx.units());
  }
  return parseValue(*def, text, ctx.units());
}

/* ---- the API struct ---- */

namespace {

UnitSettings &self(const PointerRef &p)
{
  return *static_cast<UnitSettings *>(p.data);
}

void addUnit(ApiStruct &st, const char *name, UnitFamily family)
{
  const UnitSettings defaults;
  PropertyDef def = PropertyDef::enumProp(name);
  for (int i = 0; i < UnitSettings::unitCount(family); i++) {
    def.item(defaults.unitAt(family, i).name, i);
  }
  def.defaultValue(PropValue::ofEnum(defaults.displayIndex(family)));
  ValueAccessor a;
  a.get = [family](const PointerRef &p, Context &) {
    return PropValue::ofEnum(self(p).displayIndex(family));
  };
  a.set = [family](const PointerRef &p, Context &, const PropValue &v) {
    return self(p).setDisplayIndex(family, int(v.asEnum()));
  };
  st.add(std::move(def), std::move(a));
}

} // namespace

Status registerUnitSettings(Runtime &rt)
{
  ApiRegistry &reg = rt.ext<ApiRegistry>();
  if (reg.find(StructId::of(kUnitSettingsStruct))) {
    return Status();
  }
  ApiStruct st(kUnitSettingsStruct);
  st.uiName("Units").description("How values are shown, and the frame rate.");
  addUnit(st, "lengthUnit", UnitFamily::Length);
  addUnit(st, "angleUnit", UnitFamily::Angle);
  addUnit(st, "timeUnit", UnitFamily::Time);
  addUnit(st, "massUnit", UnitFamily::Mass);
  addUnit(st, "factorUnit", UnitFamily::Factor);

  ValueAccessor fps;
  fps.get = [](const PointerRef &p, Context &) {
    return PropValue::ofFloat(self(p).rate.fps());
  };
  fps.set = [](const PointerRef &p, Context &, const PropValue &v) {
    self(p).rate = FrameRate::nearest(v.asFloat());
    return Status();
  };
  st.add(PropertyDef::floatProp("fps")
             .uiName("Frames per Second")
             .range(0.001, FrameRate::kMax)
             .softRange(1, 240)
             .defaultValue(PropValue::ofFloat(24.0)),
         std::move(fps));

  ValueAccessor start;
  start.get = [](const PointerRef &p, Context &) {
    return PropValue::ofFloat(self(p).startFrame);
  };
  start.set = [](const PointerRef &p, Context &, const PropValue &v) {
    self(p).startFrame = v.asFloat();
    return Status();
  };
  st.add(PropertyDef::floatProp("startFrame").description("The frame at time 0."),
         std::move(start));
  return reg.add(std::move(st)).status();
}

void addUnitSettings(ApiStruct &st,
                     litestl::util::function<UnitSettings *(const PointerRef &self)> get)
{
  PointerAccessor a;
  a.target = StructId::of(kUnitSettingsStruct);
  a.get = [get = std::move(get), target = a.target](const PointerRef &p, Context &) {
    return PointerRef{get(p), target, OwnerKey()};
  };
  st.addPointer(PropertyDef::pointerProp(kUnitsMember, a.target), std::move(a));
}

} // namespace coredcc

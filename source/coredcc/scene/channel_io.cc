#include "coredcc/scene/channel_io.h"

#include <cmath>
#include <utility>

namespace coredcc::detail {

using litestl::util::Vector;

int32_t channelsOf(PropKind kind)
{
  switch (kind) {
    case PropKind::Bool:
    case PropKind::Int:
    case PropKind::Float:
    case PropKind::Enum:
      return 1;
    case PropKind::Vec2:
      return 2;
    case PropKind::Vec3:
      return 3;
    case PropKind::Vec4:
    case PropKind::Quat:
      return 4;
    default:
      return 0;
  }
}

bool exactInt(int64_t v)
{
  return v >= -int64_t(channels::kMaxInt) && v <= int64_t(channels::kMaxInt);
}

void encodeChannels(const PropValue &v, int32_t first, ChannelBlock &out)
{
  switch (v.type()) {
    case PropValue::Type::Bool:
      out.set(first, v.asBool() ? 1.0 : 0.0);
      break;
    case PropValue::Type::Int:
      out.set(first, double(v.asInt()));
      break;
    case PropValue::Type::Enum:
      out.set(first, double(v.asEnum()));
      break;
    case PropValue::Type::Float:
      out.set(first, v.asFloat());
      break;
    default:
      for (int32_t i = 0; i < int32_t(v.componentCount()); i++) {
        out.set(first + i, v.component(i));
      }
      break;
  }
}

PropValue decodeChannels(const PropertyDef &def, int32_t first, const ChannelBlock &in)
{
  const double v = in.get(first);
  switch (def.kind()) {
    case PropKind::Bool:
      return PropValue::ofBool(v >= 0.5);
    case PropKind::Int:
      return PropValue::ofInt(channels::toInt(v));
    case PropKind::Float:
      return PropValue::ofFloat(v);
    case PropKind::Enum: {
      const EnumMeta &m = *def.enumMeta();
      const EnumItem *item = m.findValue(channels::toInt(v));
      return PropValue::ofEnum(item ? item->value : m.items[0].value);
    }
    default: {
      double c[4];
      for (int32_t i = 0; i < 4; i++) {
        c[i] = in.get(first + i);
      }
      return PropValue::ofComponents(valueTypeFor(def.kind()), c);
    }
  }
}

void appendChannels(const TransformType &type,
                    const ChannelBlock &channels,
                    Vector<BlockEntry> &out)
{
  int32_t first = 0;
  for (const PropertyDef &def : type.channelDefs()) {
    BlockEntry e;
    e.name = def.apiName();
    e.value = decodeChannels(def, first, channels);
    if (const std::optional<int64_t> item = e.value.tryEnum()) {
      e.value = PropValue::ofString(def.enumMeta()->findValue(*item)->key.c_str());
    }
    out.append(std::move(e));
    first += channelsOf(def.kind());
  }
}

namespace {

/* Whether the value is one of the def's, exactly. Numbers may have become of another type. */
bool take(const PropertyDef &def, const PropValue &v, int32_t first, ChannelBlock &out)
{
  switch (def.kind()) {
    case PropKind::Enum: {
      const litestl::util::string *key = v.tryString();
      const EnumItem *item = key ? def.enumMeta()->findKey(key->c_str()) : nullptr;
      if (item) {
        out.set(first, double(item->value));
      }
      return item != nullptr;
    }
    case PropKind::Int: {
      const std::optional<double> d = v.tryFloat();
      const bool whole = d && std::fabs(*d) <= channels::kMaxInt && std::floor(*d) == *d;
      if (whole || (v.tryInt() && exactInt(*v.tryInt()))) {
        out.set(first, whole ? *d + 0.0 : double(*v.tryInt()));
        return true;
      }
      return false;
    }
    case PropKind::Float: {
      const std::optional<int64_t> i = v.tryInt();
      if (i && exactInt(*i)) {
        out.set(first, double(*i));
        return true;
      }
      break;
    }
    default:
      break;
  }
  if (v.type() != valueTypeFor(def.kind())) {
    return false;
  }
  encodeChannels(v, first, out);
  return true;
}

} // namespace

ChannelBlock takeChannels(const TransformType &type, Vector<BlockEntry> &entries)
{
  ChannelBlock out = type.defaults();
  Vector<BlockEntry> unread;
  for (BlockEntry &e : entries) {
    int32_t first = 0;
    bool taken = false;
    for (const PropertyDef &def : type.channelDefs()) {
      if (def.apiName() == e.name) {
        taken = e.refs.size() == 0 && take(def, e.value, first, out);
        break;
      }
      first += channelsOf(def.kind());
    }
    if (!taken) {
      unread.append(std::move(e));
    }
  }
  entries = std::move(unread);
  return out;
}

} // namespace coredcc::detail

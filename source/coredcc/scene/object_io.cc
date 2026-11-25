/* Objects in files. */
#include "coredcc/scene/channel_entries.h"
#include "coredcc/scene/channel_io.h"
#include "coredcc/scene/object.h"
#include "coredcc/scene/payload_fields.h"

#include <string>

namespace coredcc {

using litestl::util::string;
using litestl::util::Vector;

namespace {

constexpr const char *kType = "transformType";
constexpr const char *kVersion = "transformVersion";
constexpr const char *kFlags = "flags";
constexpr const char *kParent = "parent";
constexpr const char *kData = "data";

std::span<const BlockEntry> spanOf(const Vector<BlockEntry> &v)
{
  return std::span<const BlockEntry>(v.data(), size_t(v.size()));
}

/* The name of channel `i` without a name. No def has one like it. */
std::string numberName(int i)
{
  return "#" + std::to_string(i);
}

} // namespace

/* Two lists of entries: the object's fields, and its channels, by name. They are numbered instead when
 * they have no names: the type isn't there, or has another count. */
void Object::write(BlockWriter &out) const
{
  const TransformType *type = rt_->ext<TransformRegistry>().find(type_);
  const bool pending = pending_ && unreadType_ == typeName_;
  const bool named = type && !pending && channels.size() == type->channelCount();

  Vector<BlockEntry> fields = fields_;
  fields.append(detail::valueField(kType, PropValue::ofString(typeName_.c_str())));
  const uint32_t version = named                     ? type->version() :
                           unreadType_ == typeName_ ? unreadVersion_ :
                                                      0;
  if (version) {
    fields.append(detail::valueField(kVersion, PropValue::ofInt(version)));
  }
  if (flags != ObjectFlags::None) {
    fields.append(detail::valueField(kFlags, PropValue::ofFlags(uint32_t(flags))));
  }
  if (!parent.target().isNull()) {
    fields.append(detail::refField(kParent, std::span<const RefCell>(&parent.cell(), 1)));
  }
  if (!data.target().isNull()) {
    fields.append(detail::refField(kData, std::span<const RefCell>(&data.cell(), 1)));
  }
  out.entries(spanOf(fields));

  Vector<BlockEntry> entries;
  for (const BlockEntry &e : unread()) {
    entries.append(e);
  }
  if (named) {
    detail::appendChannels(*type, channels, entries);
  }
  else {
    for (int32_t i = 0; i < channels.size(); i++) {
      entries.append(
          detail::valueField(numberName(i).c_str(), PropValue::ofFloat(channels.get(i))));
    }
  }
  out.entries(spanOf(entries));
}

void Object::readFrom(BlockReader &in)
{
  std::string type;
  uint32_t version = 0;
  detail::readFields(in, fields_, [&](const BlockEntry &e) {
    if (detail::isValueField(e, kType, PropValue::Type::String)) {
      type = e.value.asString().c_str();
      return true;
    }
    if (detail::isValueField(e, kVersion, PropValue::Type::Int) &&
        e.value.asInt() >= 0 && e.value.asInt() <= INT32_MAX)
    {
      version = uint32_t(e.value.asInt());
      return true;
    }
    if (detail::isValueField(e, kFlags, PropValue::Type::Flags)) {
      flags = ObjectFlags(uint32_t(e.value.asFlags()));
      return true;
    }
    return detail::takeRef(e, kParent, parent.cell()) ||
           detail::takeRef(e, kData, data.cell());
  });
  const TransformType *known = rt_->ext<TransformRegistry>().find(std::string_view(type));
  if (known && known->version() != version) {
    known = nullptr; /* resolve() may know what to do */
  }
  if (known) {
    channels = known->defaults();
  }
  Vector<double> numbers;
  detail::readFields(in, unread_, [&](const BlockEntry &e) {
    if (e.name.c_str()[0] != '#') {
      return known && detail::takeChannel(*known, e, channels);
    }
    const std::string next = numberName(int(numbers.size()));
    if (!detail::isValueField(e, next.c_str(), PropValue::Type::Float)) {
      return false;
    }
    numbers.append(e.value.asFloat());
    return true;
  });
  if (!in.bytes().ok()) {
    return;
  }
  setTransformType(type);
  unreadType_ = typeName_;
  unreadVersion_ = version;
  pending_ = numbers.size() == 0 && !known;
  if (numbers.size() > 0) {
    channels.resize(int32_t(numbers.size()));
    for (int32_t i = 0; i < int32_t(numbers.size()); i++) {
      channels.set(i, numbers[i]);
    }
  }
}

/* Whether it reads from `version`, as `entries`. An error is sticky: whoever asked gets it, once. */
bool Object::upgrade(const TransformType &type, Vector<BlockEntry> &entries)
{
  if (unreadVersion_ == type.version()) {
    return true;
  }
  if (unreadVersion_ > type.version() || rt_->hasStickyError()) {
    return false;
  }
  if (!type.model().hasUpgrade()) {
    return true;
  }
  ChannelEntries in(entries);
  Status s = type.model().upgrade(unreadVersion_, in);
  if (rt_->hasStickyError()) {
    s = rt_->takeStickyError();
  }
  upgradeFailed_ = !s.ok();
  if (upgradeFailed_) {
    rt_->setStickyError(Status::error(
        s.code(),
        ("upgrading the channels of a '" + std::string(type.name().c_str()) + "', version " +
         std::to_string(unreadVersion_) + ": " + s.message())
            .c_str()));
  }
  return s.ok();
}

void Object::resolve()
{
  const TransformType *type = rt_->ext<TransformRegistry>().find(type_);
  if (!pending_ || upgradeFailed_ || !type || unreadType_ != typeName_) {
    return;
  }
  Vector<BlockEntry> entries = unread_;
  if (upgrade(*type, entries)) {
    channels = detail::takeChannels(*type, entries);
    unread_ = std::move(entries);
    pending_ = false;
  }
}

uint64_t Object::memSize() const
{
  return sizeof(Object) + uint64_t(channels.size()) * sizeof(double) +
         detail::fieldsMemSize(unread_) + detail::fieldsMemSize(fields_);
}

/* ---- ChannelEntries ---- */

PropValue ChannelEntries::get(const string &name) const
{
  for (int i = int(entries_.size()) - 1; i >= 0; i--) {
    if (entries_[i].name == name) {
      return entries_[i].unknownTag ? PropValue() : entries_[i].value;
    }
  }
  return PropValue();
}

void ChannelEntries::set(const string &name, const PropValue &value)
{
  remove(name);
  entries_.append(detail::valueField(name.c_str(), value));
}

bool ChannelEntries::remove(const string &name)
{
  Vector<BlockEntry> rest;
  for (BlockEntry &e : entries_) {
    if (!(e.name == name)) {
      rest.append(std::move(e));
    }
  }
  const bool any = rest.size() < entries_.size();
  entries_ = std::move(rest);
  return any;
}

bool ChannelEntries::rename(const string &from, const string &to)
{
  if (!detail::hasField(entries_, from.c_str())) {
    return false;
  }
  if (!(from == to)) {
    remove(to);
  }
  for (BlockEntry &e : entries_) {
    if (e.name == from) {
      e.name = to;
    }
  }
  return true;
}

} // namespace coredcc

#include "coredcc/props/value_io.h"

#include "coredcc/base/assert.h"
#include "coredcc/base/text.h"

#include <cstring>
#include <string>

namespace coredcc {

namespace {

/* Form bytes. Part of the file format, as ValueTag is. */
enum class NameForm : uint8_t { Null = 0, Name = 1, RawId = 2 };
enum class OwnerForm : uint8_t { Null = 0, Block = 1, Runtime = 2 };
enum class KeyForm : uint8_t { Member = 0, StringKey = 1, Handle = 2 };

constexpr size_t kMinKeyBytes = 5;   /* a form and an empty text */
constexpr size_t kMinEntryBytes = 9; /* a name, a tag and an empty body */

PropValue::Type valueType(ValueTag tag)
{
  switch (tag) {
    case ValueTag::None:
      return PropValue::Type::None;
    case ValueTag::Bool:
      return PropValue::Type::Bool;
    case ValueTag::Int:
      return PropValue::Type::Int;
    case ValueTag::Float:
      return PropValue::Type::Float;
    case ValueTag::String:
      return PropValue::Type::String;
    case ValueTag::Vec2:
      return PropValue::Type::Vec2;
    case ValueTag::Vec3:
      return PropValue::Type::Vec3;
    case ValueTag::Vec4:
      return PropValue::Type::Vec4;
    case ValueTag::Mat4:
      return PropValue::Type::Mat4;
    case ValueTag::Quat:
      return PropValue::Type::Quat;
    case ValueTag::Enum:
      return PropValue::Type::Enum;
    case ValueTag::Flags:
      return PropValue::Type::Flags;
    case ValueTag::Pointer:
      return PropValue::Type::Pointer;
  }
  return PropValue::Type::None;
}

void writeRef(BytesWriter &out, const StableRef &ref, ValueCodec &codec)
{
  if (ref.isNull()) {
    out.u8(uint8_t(OwnerForm::Null));
    return;
  }
  if (ref.owner.kind == OwnerKey::Kind::Block) {
    out.u8(uint8_t(OwnerForm::Block));
    codec.writeBlockId(out, ref.owner.block);
  }
  else {
    out.u8(uint8_t(OwnerForm::Runtime));
    codec.writeName(out, IdDomain::Type, ref.owner.nameHash);
  }
  if (uint64_t(ref.key.size()) > kMaxKeySegments) {
    out.fail(Status::error(StatusCode::OutOfRange, "a pointer whose key is too long"));
  }
  out.u32(uint32_t(ref.key.size()));
  for (int i = 0; i < int(ref.key.size()); i++) {
    const KeySegment &s = ref.key[i];
    switch (s.kind) {
      case KeySegment::Kind::Member:
        out.u8(uint8_t(KeyForm::Member));
        out.str(s.text.c_str());
        break;
      case KeySegment::Kind::StringKey:
        out.u8(uint8_t(KeyForm::StringKey));
        out.str(s.text.c_str());
        break;
      case KeySegment::Kind::Handle:
        if (s.handle < 0) {
          out.fail(Status::error(StatusCode::InvalidArgument,
                                 "a pointer with a negative handle in its key"));
        }
        out.u8(uint8_t(KeyForm::Handle));
        out.i64(s.handle);
        break;
    }
  }
  codec.writeName(out, IdDomain::Struct, ref.type.value);
}

StableRef readRef(BytesReader &in, ValueCodec &codec)
{
  StableRef ref;
  switch (OwnerForm(in.u8())) {
    case OwnerForm::Null:
      return StableRef();
    case OwnerForm::Block:
      ref.owner.kind = OwnerKey::Kind::Block;
      codec.readBlockId(in, ref.owner.block);
      break;
    case OwnerForm::Runtime:
      ref.owner = OwnerKey::ofRuntime(codec.readName(in, IdDomain::Type));
      break;
    default:
      in.fail(StatusCode::InvalidArgument, "a pointer with an unknown kind of owner");
      return StableRef();
  }
  const uint32_t n = in.count(kMinKeyBytes);
  if (n > kMaxKeySegments) {
    in.fail(StatusCode::OutOfRange, "a pointer whose key is too long");
  }
  codec.chargeKey(in, n);
  for (uint32_t i = 0; i < n && in.ok(); i++) {
    KeySegment s;
    switch (KeyForm(in.u8())) {
      case KeyForm::Member:
        s = KeySegment::member(in.str());
        break;
      case KeyForm::StringKey:
        s = KeySegment::stringKey(in.str());
        break;
      case KeyForm::Handle: {
        const int64_t handle = in.i64();
        if (handle < 0) {
          in.fail(StatusCode::InvalidArgument, "a negative handle in a pointer's key");
          return StableRef();
        }
        s = KeySegment::ofHandle(handle);
        break;
      }
      default:
        in.fail(StatusCode::InvalidArgument, "an unknown kind of key segment");
        return StableRef();
    }
    ref.key.append(std::move(s));
  }
  ref.type.value = codec.readName(in, IdDomain::Struct);
  return ref;
}

} // namespace

/* ---- tags ---- */

ValueTag valueTag(PropValue::Type type)
{
  switch (type) {
    case PropValue::Type::None:
      return ValueTag::None;
    case PropValue::Type::Bool:
      return ValueTag::Bool;
    case PropValue::Type::Int:
      return ValueTag::Int;
    case PropValue::Type::Float:
      return ValueTag::Float;
    case PropValue::Type::String:
      return ValueTag::String;
    case PropValue::Type::Vec2:
      return ValueTag::Vec2;
    case PropValue::Type::Vec3:
      return ValueTag::Vec3;
    case PropValue::Type::Vec4:
      return ValueTag::Vec4;
    case PropValue::Type::Mat4:
      return ValueTag::Mat4;
    case PropValue::Type::Quat:
      return ValueTag::Quat;
    case PropValue::Type::Enum:
      return ValueTag::Enum;
    case PropValue::Type::Flags:
      return ValueTag::Flags;
    case PropValue::Type::Pointer:
      return ValueTag::Pointer;
  }
  return ValueTag::None;
}

bool isKnownValueTag(uint8_t tag)
{
  return tag <= uint8_t(ValueTag::Pointer);
}

/* ---- ValueCodec ---- */

void ValueCodec::writeBlockId(BytesWriter &out, const BlockId &id)
{
  writeName(out, IdDomain::Client, id.client.value);
  out.u64(id.local.hi);
  out.u64(id.local.lo);
}

void ValueCodec::readBlockId(BytesReader &in, BlockId &id)
{
  id.client.value = readName(in, IdDomain::Client);
  id.local.hi = in.u64();
  id.local.lo = in.u64();
}

void ValueCodec::chargeKey(BytesReader &, uint32_t) {}

void ValueCodec::writeName(BytesWriter &out, IdDomain domain, uint64_t id) const
{
  if (id == 0) {
    out.u8(uint8_t(NameForm::Null));
    return;
  }
  const char *name = rt_.nameOf(domain, id);
  if (!name && names_) {
    name = names_->nameOf(domain, id);
  }
  /* A name that doesn't hash to its id (a collision a test forced) can't stand for it. */
  if (name && hashId(domain, name) == id) {
    out.u8(uint8_t(NameForm::Name));
    out.str(name);
    return;
  }
  out.u8(uint8_t(NameForm::RawId));
  out.u64(id);
}

uint64_t ValueCodec::readName(BytesReader &in, IdDomain domain)
{
  switch (NameForm(in.u8())) {
    case NameForm::Null:
      return 0;
    case NameForm::Name: {
      const std::string_view name = in.str();
      const uint64_t id = hashId(domain, name);
      if (name.empty() || id == 0) {
        in.fail(StatusCode::InvalidArgument, "an empty name");
        return 0;
      }
      const char *known = rt_.nameOf(domain, id);
      if (!known && names_ && !names_->add(domain, name)) {
        known = names_->nameOf(domain, id); /* null when the table is full */
      }
      if (known && name != known) {
        in.fail(StatusCode::InvalidArgument,
                (std::string("a name with the same id as '") + known + "'").c_str());
        return 0;
      }
      return id;
    }
    case NameForm::RawId: {
      const uint64_t id = in.u64();
      if (id == 0) {
        in.fail(StatusCode::InvalidArgument, "a null id written as a raw one");
      }
      return id;
    }
  }
  in.fail(StatusCode::InvalidArgument, "an unknown form of name");
  return 0;
}

/* ---- values ---- */

void writeValueBody(BytesWriter &out, const PropValue &v, ValueCodec &codec)
{
  switch (v.type()) {
    case PropValue::Type::None:
      break;
    case PropValue::Type::Bool:
      out.boolean(v.asBool());
      break;
    case PropValue::Type::Int:
      out.i64(v.asInt());
      break;
    case PropValue::Type::String:
      out.str(v.asString().c_str());
      break;
    case PropValue::Type::Float:
    case PropValue::Type::Vec2:
    case PropValue::Type::Vec3:
    case PropValue::Type::Vec4:
    case PropValue::Type::Mat4:
    case PropValue::Type::Quat:
      for (int i = 0; i < v.componentCount(); i++) {
        out.f64(v.component(i));
      }
      break;
    case PropValue::Type::Enum:
      out.i64(v.asEnum());
      break;
    case PropValue::Type::Flags:
      out.u64(v.asFlags());
      break;
    case PropValue::Type::Pointer:
      writeRef(out, v.asPointer(), codec);
      break;
  }
}

PropValue readValueBody(BytesReader &in, ValueTag tag, ValueCodec &codec)
{
  PropValue v;
  switch (tag) {
    case ValueTag::None:
      break;
    case ValueTag::Bool:
      v = PropValue::ofBool(in.boolean());
      break;
    case ValueTag::Int:
      v = PropValue::ofInt(in.i64());
      break;
    case ValueTag::Float:
      v = PropValue::ofFloat(in.f64());
      break;
    case ValueTag::String:
      v = PropValue::ofString(in.str());
      break;
    case ValueTag::Vec2:
    case ValueTag::Vec3:
    case ValueTag::Vec4:
    case ValueTag::Mat4:
    case ValueTag::Quat: {
      const PropValue::Type type = valueType(tag);
      double c[16] = {};
      for (int i = 0; i < valueComponentCount(type); i++) {
        c[i] = in.f64();
      }
      v = PropValue::ofComponents(type, c);
      break;
    }
    case ValueTag::Enum:
      v = PropValue::ofEnum(in.i64());
      break;
    case ValueTag::Flags:
      v = PropValue::ofFlags(in.u64());
      break;
    case ValueTag::Pointer:
      v = PropValue::ofPointer(readRef(in, codec));
      break;
    default:
      in.fail(StatusCode::Unsupported, "a value of an unknown type");
      break;
  }
  return in.ok() ? v : PropValue();
}

void writeValue(BytesWriter &out, const PropValue &v, ValueCodec &codec)
{
  out.u8(uint8_t(valueTag(v.type())));
  writeValueBody(out, v, codec);
}

PropValue readValue(BytesReader &in, ValueCodec &codec)
{
  return readValueBody(in, ValueTag(in.u8()), codec);
}

/* ---- bags ---- */

PropValue storedBagValue(const PropertyBag &bag, int i)
{
  const PropValue &v = bag.value(i);
  if (v.type() != PropValue::Type::Enum && v.type() != PropValue::Type::Flags) {
    return v;
  }
  const PropertyDef def = bag.def(i).resolved(&bag);
  const EnumMeta *meta = def.enumMeta();
  if (!meta) {
    return v;
  }
  if (v.type() == PropValue::Type::Enum) {
    const EnumItem *item = meta->findValue(v.asEnum());
    return item ? PropValue::ofString(item->key.c_str()) : v;
  }
  std::string keys;
  uint64_t left = v.asFlags();
  for (const EnumItem &item : meta->items) {
    const uint64_t bit = uint64_t(item.value);
    if (left & bit) {
      keys += keys.empty() ? "" : "|";
      keys += item.key.c_str();
      left &= ~bit;
    }
  }
  return left == 0 ? PropValue::ofString(keys) : v;
}

void writeBag(BytesWriter &out, const PropertyBag &bag, ValueCodec &codec)
{
  uint32_t n = 0;
  for (int i = 0; i < bag.size(); i++) {
    n += bag.wasSet(i) ? 1 : 0;
  }
  out.u32(n);
  for (int i = 0; i < bag.size(); i++) {
    if (!bag.wasSet(i)) {
      continue;
    }
    const PropValue v = storedBagValue(bag, i);
    out.str(bag.def(i).apiName().c_str());
    out.u8(uint8_t(valueTag(v.type())));
    const size_t body = out.beginSection();
    writeValueBody(out, v, codec);
    out.endSection(body);
  }
}

void readBag(BytesReader &in,
             PropertyBag &bag,
             ValueCodec &codec,
             litestl::util::Vector<BagIssue> &issues)
{
  const auto issue = [&issues](std::string_view name, Status why) {
    if (int(issues.size()) < kMaxBagIssues) {
      issues.append(BagIssue{lsString(name), std::move(why)});
    }
  };
  const uint32_t n = in.count(kMinEntryBytes);
  for (uint32_t i = 0; i < n && in.ok(); i++) {
    const std::string_view name = in.str();
    const uint8_t tag = in.u8();
    BytesReader body(in.blob());
    if (!in.ok()) {
      return;
    }
    if (!isKnownValueTag(tag)) {
      issue(name, Status::error(StatusCode::Unsupported, "a value of an unknown type"));
      continue;
    }
    const PropValue v = readValueBody(body, ValueTag(tag), codec);
    if (body.ok() && !body.atEnd()) {
      body.fail(StatusCode::InvalidArgument, "a value with bytes left over");
    }
    if (!body.ok()) {
      in.fail(body.status());
      return;
    }
    const int at = bag.indexOf(name);
    Status s = at < 0 ? Status::error(StatusCode::NotFound, "no such property") : bag.setAt(at, v);
    if (!s.ok()) {
      issue(name, std::move(s));
    }
  }
}

} // namespace coredcc

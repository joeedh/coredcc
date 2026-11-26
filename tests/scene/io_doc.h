#pragma once

/* Documents that are saved and loaded, and payloads made by hand. */

#include "command_doc.h"

#include "coredcc/api/library_io.h"
#include "coredcc/lib/payload_access.h"
#include "coredcc/scene/payload_fields.h"

#include <cstring>
#include <string>

namespace scene_test {

using Bytes = Vector<uint8_t>;
using detail::PayloadAccess;

inline std::span<const uint8_t> view(const Bytes &b)
{
  return std::span<const uint8_t>(b.data(), size_t(b.size()));
}
inline bool same(const Bytes &a, const Bytes &b)
{
  return a.size() == b.size() && memcmp(a.data(), b.data(), size_t(a.size())) == 0;
}
inline bool sameBits(double a, double b)
{
  return memcmp(&a, &b, sizeof(a)) == 0;
}
inline Bytes save(const Library &lib)
{
  SaveReport report;
  Result<Bytes> b = saveLibrary(lib, &report);
  must(b.status());
  test_assert(report.issues.total == 0);
  return *b;
}

/* A transform type with a channel of each kind. `version` 1 has other channels: `size` is
 * an Int, `count` a Float, `mode` has other items and numbers, and `extra` isn't there.
 */
inline TransformTypeInfo everyKind(int version)
{
  TransformTypeInfo info;
  info.name = "io.type";
  info.channels.append(PropertyDef::boolProp("on"));
  PropertyDef size =
      version ? PropertyDef::intProp("size") : PropertyDef::floatProp("size");
  info.channels.append(size);
  info.channels.append(version ? PropertyDef::floatProp("count")
                               : PropertyDef::intProp("count"));
  const int base = version ? 20 : 0;
  PropertyDef mode = PropertyDef::enumProp("mode");
  if (!version) {
    mode.item("ONE", 1)
        .item("TWO", 2)
        .item("THREE", 3)
        .defaultValue(PropValue::ofEnum(1));
  } else {
    mode.item("TWO", base + 2)
        .item("ONE", base + 1)
        .defaultValue(PropValue::ofEnum(base + 2));
  }
  info.channels.append(std::move(mode));
  if (!version) {
    info.channels.append(PropertyDef::vecProp("extra", 3));
  }
  info.channels.append(PropertyDef::vecProp("at", 2));
  info.channels.append(PropertyDef::vecProp("four", 4));
  info.channels.append(PropertyDef::quatProp("turn"));
  return info;
}

struct IoDoc : CommandDoc {
  IoDoc()
  {
    lib->setAuditRelations(true);
  }
  void addEveryKind(int version = 0, const char *name = "io.type")
  {
    const std::string implName = std::string(name) + "#impl";
    const char *impl = implName.c_str();
    must(registerImpl<TransformModel, FnModel>(rt, impl));
    FnModel *m = litestl::alloc::New<FnModel>("FnModel");
    m->fn = [](const EvalContext &, const TransformState &p) { return p; };
    Result<Owned<TransformModel>> owned = own<TransformModel>(rt, impl, m);
    must(owned.status());
    TransformTypeInfo info = everyKind(version);
    info.name = name;
    must(rt.ext<TransformRegistry>().add(std::move(info), std::move(owned).value()).status());
  }
  BlockHandle byName(const char *name)
  {
    BlockHandle found;
    lib->forEach([&](BlockHandle h, const Datablock &b) {
      if (strcmp(b.name().c_str(), name) == 0 && b.state() == BlockState::Live) {
        found = h;
      }
    });
    return found;
  }
};

/* Loads into a library of its own. */
struct Loaded {
  Library lib;
  Context ctx;
  LoadReport report;

  Loaded(IoDoc &d, const Bytes &bytes, int64_t issues = 0)
      : lib(&d.rt), ctx(&d.rt, &lib, d.ctx->root())
  {
    lib.setAuditRelations(true);
    Result<LoadReport> r = loadLibrary(ctx, view(bytes));
    must(r.status());
    report = *r;
    for (const IoIssue &i : report.issues.first) {
      if (issues == 0) {
        printf("issue: %s\n", i.why.toString().c_str());
      }
    }
    test_assert(report.issues.total == issues);
    ctx.setScene(byName("Scene"));
  }
  BlockHandle byName(const char *name) const
  {
    BlockHandle found;
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (strcmp(b.name().c_str(), name) == 0 && b.state() == BlockState::Live) {
        found = h;
      }
    });
    return found;
  }
  Object &obj(const char *name) const
  {
    const BlockHandle h = byName(name);
    test_assert(bool(h));
    return *static_cast<Object *>(lib.get(h)->clientData());
  }
  Scene &sc() const
  {
    return *Scene::of(lib, byName("Scene"));
  }
};

inline bool sameChannels(const ChannelBlock &a, const ChannelBlock &b)
{
  bool same = a.size() == b.size();
  for (int i = 0; i < a.size() && same; i++) {
    same = sameBits(a.get(i), b.get(i));
  }
  return same;
}

/* Entries of a build that doesn't exist: of a type of value, or with references. */
struct Fields {
  IoDoc &d;
  BlockWriter out;
  Vector<BlockEntry> fields, entries;

  explicit Fields(IoDoc &doc) : d(doc), out(*doc.lib)
  {
  }
  BlockEntry &entry(const char *name, PropValue v)
  {
    BlockEntry e;
    e.name = name;
    e.value = std::move(v);
    entries.append(std::move(e));
    return entries[int(entries.size()) - 1];
  }
  BlockPayload payload(const char *type = kTrsTransform)
  {
    fields.append(detail::valueField("transformType", PropValue::ofString(type)));
    out.entries(std::span<const BlockEntry>(fields.data(), size_t(fields.size())));
    out.entries(std::span<const BlockEntry>(entries.data(), size_t(entries.size())));
    Result<BlockPayload> p = PayloadAccess::take(out, 1);
    must(p.status());
    return *p;
  }
};

inline Status
readInto(IoDoc &d, const char *type, const BlockPayload &p, BlockHandle *out = nullptr)
{
  const BlockId id{ClientId::of(kCoredccClient), d.rt.newLocalId()};
  Result<BlockHandle> h = d.lib->createWithId(TypeId::of(type), id, "read", nullptr);
  must(h.status());
  const Status s = readPayload(*d.lib, *h, p);
  test_assert(s.ok() == (d.lib->get(*h)->clientData() != nullptr));
  if (out) {
    *out = *h;
  }
  return s;
}

} // namespace scene_test

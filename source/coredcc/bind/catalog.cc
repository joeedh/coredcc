#include "coredcc/bind/catalog.h"

#include "coredcc/api/struct.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/meta.h"
#include "coredcc/command/command.h"

namespace coredcc {

namespace {

void text(JsonWriter &out, const char *key, const litestl::util::string &s)
{
  if (s.size() > 0) {
    out.key(key).string(s.c_str());
  }
}

void writeMember(JsonWriter &out, const Runtime &rt, const ApiMember &m)
{
  out.beginObject();
  out.key("kind").string(propKindName(m.def().kind()));
  out.key("readOnly").boolean(m.readOnly());
  if (m.kind() == MemberKind::Pointer && !m.pointer().target) {
    out.key("targets").beginArray();
    for (StructId id : m.pointer().candidates) {
      if (const char *name = rt.nameOf(id)) {
        out.string(name);
      }
    }
    out.endArray();
  }
  if (m.kind() == MemberKind::Collection) {
    out.key("active").boolean(bool(m.collection().active));
  }
  writePropertyMeta(out, rt, m.def());
  out.endObject();
}

void writeStruct(JsonWriter &out, const Runtime &rt, const ApiStruct &s)
{
  out.beginObject();
  out.key("name").string(s.name().c_str());
  out.key("uiName").string(s.uiNameText().c_str());
  text(out, "description", s.descriptionText());
  if (const char *parent = s.parentId() ? rt.nameOf(s.parentId()) : nullptr) {
    out.key("parent").string(parent);
  }
  out.key("members").beginArray();
  for (int i = 0; i < s.memberCount(); i++) {
    if (!s.member(i).def().hasAccess(PropAccess::Private)) {
      writeMember(out, rt, s.member(i));
    }
  }
  out.endArray().endObject();
}

void writeDef(JsonWriter &out, const Runtime &rt, const PropertyDef &def)
{
  if (!def.hasAccess(PropAccess::Private)) {
    out.beginObject();
    out.key("kind").string(propKindName(def.kind()));
    writePropertyMeta(out, rt, def);
    out.endObject();
  }
}

void writeCommand(JsonWriter &out, const Runtime &rt, const CommandDef &def)
{
  static constexpr struct {
    CommandFlags flag;
    const char *name;
  } kFlags[] = {
      {CommandFlags::NoUndo, "NoUndo"},
      {CommandFlags::UndoBarrier, "UndoBarrier"},
      {CommandFlags::Interactive, "Interactive"},
  };
  out.beginObject();
  out.key("id").string(def.name().c_str());
  out.key("uiName").string(def.uiNameText().c_str());
  text(out, "description", def.descriptionText());
  text(out, "icon", def.iconText());
  out.key("flags").beginArray();
  for (const auto &f : kFlags) {
    if (def.has(f.flag)) {
      out.string(f.name);
    }
  }
  out.endArray();
  out.key("inputs").beginArray();
  for (int i = 0; i < def.inputCount(); i++) {
    writeDef(out, rt, def.inputDef(i));
  }
  out.endArray().key("outputs").beginArray();
  for (int i = 0; i < def.outputCount(); i++) {
    writeDef(out, rt, def.outputDef(i));
  }
  out.endArray().endObject();
}

} // namespace

std::string apiCatalog(Context &ctx)
{
  Runtime &rt = ctx.runtime();
  const ApiRegistry &reg = rt.ext<ApiRegistry>();
  const CommandRegistry &commands = rt.ext<CommandRegistry>();
  JsonWriter out;
  out.beginObject();
  if (const ApiStruct *root = reg.find(ctx.root().type)) {
    out.key("root").string(root->name().c_str());
  }
  out.key("structs").beginArray();
  for (int i = 0; i < reg.structCount(); i++) {
    writeStruct(out, rt, reg.structAt(i));
  }
  out.endArray().key("commands").beginArray();
  for (int i = 0; i < commands.size(); i++) {
    if (!commands.at(i).has(CommandFlags::Private)) {
      writeCommand(out, rt, commands.at(i));
    }
  }
  out.endArray().endObject();
  return out.take();
}

namespace script {

StringResult apiCatalog(Context &ctx)
{
  if (Status s = detail::kernelGate(ctx, "apiCatalog"); !s.ok()) {
    return detail::failed<StringResult>(std::move(s));
  }
  return detail::ok<StringResult>(string(coredcc::apiCatalog(ctx).c_str()));
}

} // namespace script
} // namespace coredcc

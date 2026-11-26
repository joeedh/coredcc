#include "coredcc/bind/drive_facades.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"
#include "coredcc/drive/drive.h"
#include "coredcc/graph/node_type.h"
#include "coredcc/scene/commands.h"
#include "coredcc/scene/scene.h"

#include <memory>
#include <vector>

namespace coredcc::script {

using detail::failed;
using detail::kernelGate;
using detail::slotGate;

namespace {

/* The Runtime's SceneDrivers, one per Library. Made after the node types, so it goes
 * before them; a Library that went first leaves an entry that does nothing. */
class ScriptDrivers {
public:
  static constexpr const char *kExtName = "coredcc.script.ScriptDrivers";
  explicit ScriptDrivers(Runtime *rt)
  {
    rt->ext<NodeTypeRegistry>();
  }
  struct Entry {
    LibraryRef lib;
    std::unique_ptr<SceneDrivers> drivers;
  };
  std::vector<Entry> entries;

  SceneDrivers *find(const Library *lib)
  {
    std::erase_if(entries, [](const Entry &e) { return !e.lib.get(); });
    for (Entry &e : entries) {
      if (e.lib.get() == lib) {
        return e.drivers.get();
      }
    }
    return nullptr;
  }
};

/* A pointer's text, as TS's Ref has it; "" for none. */
std::string graphRef(const Context &ctx, const BlockId &id)
{
  const Library &lib = *ctx.library();
  const BlockHandle h = lib.find(id);
  if (!lib.get(h)) {
    return "";
  }
  return formatRef(ctx.runtime(), *coredcc::detail::pointerTo(lib, h).tryPointer());
}

/* `3/7`; a path that ends at a group, with a null node, is the groups alone. */
std::string nodeText(const NodePath &p)
{
  if (!p.node.isNull()) {
    return p.toString();
  }
  std::string s;
  for (NodeId g : p.groups) {
    s += (s.empty() ? "" : "/") + std::to_string(g.value);
  }
  return s;
}

Status noLibrary(const char *what)
{
  return Status::error(StatusCode::FailedPrecondition,
                       (std::string(what) + ": the Context has no Library").c_str());
}

} // namespace

Status enableDrivers(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "enableDrivers", {}));
  Library *lib = ctx.library();
  if (!lib) {
    return noLibrary("enableDrivers");
  }
  ScriptDrivers &all = ctx.runtime().ext<ScriptDrivers>();
  if (all.find(lib) || SceneDrivers::of(*lib)) {
    return Status::error(StatusCode::AlreadyExists, "enableDrivers: they are enabled");
  }
  Result<std::unique_ptr<SceneDrivers>> d = SceneDrivers::make(*lib);
  CDCC_TRY(d.status());
  all.entries.push_back(ScriptDrivers::Entry{lib->ref(), std::move(d).value()});
  return Status();
}

Status disableDrivers(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "disableDrivers", {}));
  CDCC_TRY(ctx.runtime().busyIfEvaluating("disableDrivers"));
  ScriptDrivers &all = ctx.runtime().ext<ScriptDrivers>();
  const Library *lib = ctx.library();
  std::erase_if(all.entries, [&](const ScriptDrivers::Entry &e) {
    return !e.lib.get() || e.lib.get() == lib;
  });
  return Status();
}

StringResult driverDiagnostics(Context &ctx)
{
  if (Status s = kernelGate(ctx, "driverDiagnostics"); !s.ok()) {
    return failed<StringResult>(s);
  }
  SceneDrivers *d = ctx.library() ? SceneDrivers::of(*ctx.library()) : nullptr;
  if (!d) {
    return failed<StringResult>(Status::error(
        StatusCode::FailedPrecondition, "driverDiagnostics: drivers aren't enabled"));
  }
  const Scene *scene = activeScene(ctx);
  if (Status s = d->update(scene ? scene->time() : 0.0);
      !s.ok() && s.code() == StatusCode::Busy)
  {
    return failed<StringResult>(s);
  }
  JsonWriter out;
  out.beginArray();
  for (const DriverDiagnostic &dg : d->diagnostics()) {
    out.beginObject();
    out.key("graph").string(graphRef(ctx, dg.graph));
    out.key("node").string(nodeText(dg.node));
    out.key("message").string(dg.message.c_str());
    out.endObject();
  }
  out.endArray();
  StringResult r;
  r.value = out.take().c_str();
  return r;
}

} // namespace coredcc::script

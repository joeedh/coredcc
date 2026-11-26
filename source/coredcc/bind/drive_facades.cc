#include "coredcc/bind/drive_facades.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"
#include "coredcc/drive/drive.h"
#include "coredcc/graph/node_type.h"
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
  if (all.find(lib)) {
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
  SceneDrivers *d =
      ctx.library() ? ctx.runtime().ext<ScriptDrivers>().find(ctx.library()) : nullptr;
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
    out.key("graph").string(dg.graph.isNull() ? ""
                                              : formatBlockId(ctx.runtime(), dg.graph));
    out.key("node").string(dg.node.node.isNull() ? "" : dg.node.toString());
    out.key("message").string(dg.message.c_str());
    out.endObject();
  }
  out.endArray();
  StringResult r;
  r.value = out.take().c_str();
  return r;
}

} // namespace coredcc::script

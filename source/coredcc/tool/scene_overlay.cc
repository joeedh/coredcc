#include "coredcc/tool/scene_overlay.h"

#include "coredcc/draw/overlay.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/evaluated.h"
#include "coredcc/scene/scene.h"

#include "litestl/util/map.h"

namespace coredcc {

Status drawSceneOverlays(Context &ctx,
                         BlockHandle sceneHandle,
                         const ViewMath &view,
                         DrawList &out)
{
  Library *lib = ctx.library();
  Scene *scene = lib && sceneHandle ? Scene::of(*lib, sceneHandle) : nullptr;
  if (!scene) {
    return Status();
  }
  Runtime &rt = ctx.runtime();
  litestl::util::Map<uint64_t, bool> selected;
  for (BlockHandle h : scene->selected(*lib)) {
    selected.add(h.computeHash(), true);
  }
  const BlockHandle active = scene->active(*lib);
  const auto &objects = scene->evaluatedObjects();
  const std::span<const ObjectEval> results = scene->results();
  Status first;
  for (int i = 0; i < int(objects.size()) && i < int(results.size()); i++) {
    const ObjectEval &ev = results[i];
    if (!ev.member || ev.status != EvalStatus::Ok ||
        (ev.flags & uint32_t(ObjectFlags::Hidden)) != 0 || !lib->get(objects[i]))
    {
      continue;
    }
    const OverlayDrawable d = objectQuery<OverlayDrawable>(*lib, objects[i]);
    if (!d) {
      continue;
    }
    uint32_t state = 0;
    state |= selected.lookup_ptr(objects[i].computeHash())
                 ? uint32_t(OverlayState::Selected)
                 : 0;
    state |= objects[i] == active ? uint32_t(OverlayState::Active) : 0;
    if (!out.pushMatrix(ev.dataWorld()).ok()) {
      continue; /* a matrix that isn't affine or finite: nothing to draw it in */
    }
    Status s;
    {
      /* The slot can't pop the object's push; what it leaves pushed is popped. */
      DrawSlotScope slot(out);
      d.drawOverlay(view, state, out);
      s = rt.hasStickyError() ? rt.takeStickyError() : Status();
      const Status left = slot.close("an object's drawOverlay");
      s = s.ok() ? left : s;
    }
    const Status popped = out.popMatrix();
    s = s.ok() ? popped : s;
    first = first.ok() ? s : first;
  }
  return first;
}

} // namespace coredcc

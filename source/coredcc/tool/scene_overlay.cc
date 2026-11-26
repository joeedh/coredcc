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
    const int32_t depth = out.depth();
    d.drawOverlay(view, state, out);
    if (rt.hasStickyError()) {
      Status err = rt.takeStickyError();
      first = first.ok() ? err : first;
    }
    while (out.depth() > depth) {
      out.popMatrix().ignore(); /* what the slot pushed and left */
    }
    out.popMatrix().ignore();
  }
  return first;
}

} // namespace coredcc

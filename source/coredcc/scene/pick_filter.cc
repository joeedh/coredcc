#include "coredcc/scene/pick_filter.h"

#include "coredcc/api/context.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/scene.h"

#include <string>

namespace coredcc::detail {

SceneQuery::SceneQuery(Context &ctx, BlockHandle scene, const char *what)
    : rt_(ctx.runtime())
{
  status_ = begin(ctx, scene, what);
  if (status_.ok()) {
    /* As evaluateScene does, so that what a capability does can't take the scene away. */
    evaluating_ = std::make_unique<EvaluationScope>(rt_);
    pin_ = LibraryPin(*lib_, scene);
    lib_->beginNoCollect();
  }
}

SceneQuery::~SceneQuery()
{
  if (evaluating_) {
    lib_->endNoCollect();
  }
}

Status SceneQuery::begin(Context &ctx, BlockHandle scene, const char *what)
{
  const auto error = [what](StatusCode code, const char *why) {
    return Status::error(code, (std::string(what) + ": " + why).c_str());
  };
  lib_ = ctx.library();
  if (!lib_) {
    return error(StatusCode::FailedPrecondition, "the Context has no Library");
  }
  CDCC_TRY(rt_.busyIfEvaluating(what));
  if (lib_->busy()) {
    return error(StatusCode::Busy, "the library is busy");
  }
  if (rt_.hasStickyError()) {
    return error(StatusCode::FailedPrecondition, "an error is pending");
  }
  scene_ = Scene::of(*lib_, scene);
  if (!scene_) {
    return error(StatusCode::NotFound, "no such scene");
  }
  return scene_->bvh().valid()
             ? Status()
             : error(StatusCode::FailedPrecondition, "the scene hasn't been evaluated");
}

bool SceneQuery::thrown()
{
  if (!rt_.hasStickyError()) {
    return false;
  }
  Status s = rt_.takeStickyError();
  if (foreign_.ok()) {
    foreign_ = std::move(s);
  }
  return true;
}

Spatial SceneQuery::spatial(BlockHandle h, const PickFilter &filter)
{
  if (filter.accept && !filter.accept(h)) {
    return Spatial();
  }
  ObjectInfo info;
  const Transformable t = lib_->query<Transformable>(h);
  if (thrown()) {
    return Spatial();
  }
  if (t) {
    ChannelBlock channels;
    t.read(info, channels);
    if (thrown()) {
      return Spatial();
    }
  }
  const ObjectFlags flags = ObjectFlags(info.flags);
  if ((hasFlag(flags, ObjectFlags::Hidden) && !filter.hidden) ||
      (hasFlag(flags, ObjectFlags::Unselectable) && !filter.unselectable))
  {
    return Spatial();
  }
  Spatial s =
      info.data.isNull() ? Spatial() : lib_->query<Spatial>(lib_->find(info.data));
  if (thrown()) {
    return Spatial();
  }
  if (!s) {
    s = lib_->query<Spatial>(h);
  }
  return thrown() ? Spatial() : s;
}

} // namespace coredcc::detail

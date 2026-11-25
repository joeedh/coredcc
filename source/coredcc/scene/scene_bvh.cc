#include "coredcc/scene/scene_bvh.h"

namespace coredcc {

Status SceneBvh::update(const detail::EvaluatedStore &results)
{
  BoxSource source;
  source.ensureCapacity(int(results.objects.size()));
  for (const ObjectEval &e : results.objects) {
    source.append(e.member && e.status != EvalStatus::Invalid ? e.bounds : Box3d());
  }
  const bool sameObjects = index_ == results.index;
  index_ = results.index;
  if (sameObjects && boxes_.refit(source).ok()) {
    if (boxes_.bvh().cost() <= 2.0 * builtCost_) {
      counters_.refits++;
      return Status();
    }
  }
  CDCC_TRY(boxes_.build(source));
  counters_.builds++;
  builtCost_ = boxes_.bvh().cost();
  return Status();
}

} // namespace coredcc

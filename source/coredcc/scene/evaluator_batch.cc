/* Evaluating objects of one type and level together (TransformModel::evaluateMany). */
#include "coredcc/lib/pin.h"
#include "coredcc/scene/evaluator.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::util::Vector;

Vector<double> TransformBatch::channels() const
{
  return channels_;
}

Vector<double> TransformBatch::parents() const
{
  Vector<double> out;
  out.ensure_capacity(parents_.size() * kStateSize);
  for (const TransformState &s : parents_) {
    const double *m = s.world;
    for (int i = 0; i < 16; i++) {
      out.append(m[i]);
    }
    for (int i = 0; i < 4; i++) {
      out.append(s.rotation[i]);
    }
    for (int i = 0; i < 3; i++) {
      out.append(s.localScale[i]);
    }
  }
  return out;
}

Status TransformBatch::setStates(const Vector<double> &states)
{
  if (int(states.size()) != count() * kStateSize) {
    return Status::error(StatusCode::InvalidArgument,
                         "evaluateMany: there must be 23 numbers for each object");
  }
  states_.clear();
  for (int k = 0; k < count(); k++) {
    const double *from = states.data() + size_t(k) * kStateSize;
    TransformState s;
    double *m = s.world;
    for (int i = 0; i < 16; i++) {
      m[i] = from[i];
    }
    for (int i = 0; i < 4; i++) {
      s.rotation[i] = from[16 + i];
    }
    for (int i = 0; i < 3; i++) {
      s.localScale[i] = from[20 + i];
    }
    states_.append(s);
  }
  return Status();
}

lbt::Struct<TransformBatch> *TransformBatch::defineBindings()
{
  static lbt::Struct<TransformBatch> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformBatch>("coredcc::TransformBatch",
                                         sizeof(TransformBatch));
    BIND_STRUCT_METHOD(st, count, MARGS());
    BIND_STRUCT_METHOD(st, channelCount, MARGS());
    BIND_STRUCT_METHOD(st, time, MARGS());
    BIND_STRUCT_METHOD(st, channels, MARGS());
    BIND_STRUCT_METHOD(st, parents, MARGS());
    BIND_STRUCT_METHOD(st, setStates, MARGS("states"));
  }
  return st;
}

namespace detail {

namespace {

struct Group {
  const TransformType *type = nullptr;
  Vector<int> members;
  std::unique_ptr<TransformBatch> batch;
};

} // namespace

/* What can't be read here, for whatever reason, is left to the pass over all. */
void SceneEvaluator::evaluateLevel(const Library &lib,
                                   int from,
                                   int to,
                                   OverrideProvider *provider,
                                   double time,
                                   const Vector<ObjectEval> &results,
                                   Status &foreign)
{
  Runtime &rt = lib.runtime();
  const TransformRegistry &types = rt.ext<TransformRegistry>();
  const TransformState root;
  Vector<Group> groups;
  const auto parentOf = [&](int i) -> const TransformState & {
    const int p = members_[i].parent;
    return p < 0 || cyclic_[i] ? root : results[p].state;
  };
  for (int k = from; k < to; k++) {
    const int i = order_[k];
    const SceneMember &m = members_[i];
    batch_[i].done = false;
    const TransformType *type = types.find(m.transformType);
    if (!type || !type->model().hasEvaluateMany()) {
      continue;
    }
    const Transformable tr = lib.query<Transformable>(m.handle);
    ObjectInfo info;
    if (tr) {
      tr.read(info, scratch_);
    }
    if (!tr || rt.hasStickyError() || info.transformType != m.transformType ||
        scratch_.size() != type->channelCount())
    {
      if (rt.hasStickyError() && foreign.ok()) {
        foreign = rt.takeStickyError();
      }
      continue;
    }
    OverrideProvider *const asked = providerFor(i, provider);
    if (asked && !overridesOf(asked, lib, m.handle, type, time, overrides_).ok()) {
      continue;
    }
    Group *group = nullptr;
    for (Group &g : groups) {
      group = g.type == type ? &g : group;
    }
    if (!group) {
      groups.append(Group());
      group = &groups[int(groups.size()) - 1];
      group->type = type;
      group->batch = std::make_unique<TransformBatch>(type->channelCount(), time);
    }
    const EvalContext ctx =
        asked ? EvalContext(scratch_, time, overrides_.values, overrides_.mask)
              : EvalContext(scratch_, time);
    group->batch->add(ctx, parentOf(i));
    group->members.append(i);
    batch_[i].info = info;
  }
  for (Group &g : groups) {
    Status s = g.type->model().evaluateMany(*g.batch);
    if (rt.hasStickyError()) {
      s = rt.takeStickyError();
    }
    if (s.ok() && int(g.batch->states().size()) != g.batch->count()) {
      s = Status::error(StatusCode::Internal, "evaluateMany gave no states");
    }
    if (!s.ok() && foreign.ok()) {
      foreign = s;
    }
    for (int k = 0; k < int(g.members.size()); k++) {
      BatchItem &item = batch_[g.members[k]];
      item.done = true;
      item.status = !s.ok()                          ? EvalStatus::ModelFailed
                    : !g.batch->states()[k].finite() ? EvalStatus::Invalid
                                                     : EvalStatus::Ok;
      item.state =
          item.status == EvalStatus::Ok ? g.batch->states()[k] : parentOf(g.members[k]);
    }
  }
}

} // namespace detail
} // namespace coredcc

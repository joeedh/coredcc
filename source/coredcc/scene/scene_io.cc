/* Scenes in files. */
#include "coredcc/scene/payload_fields.h"
#include "coredcc/scene/scene.h"

#include "litestl/util/alloc.h"

#include <cmath>

namespace coredcc {

using litestl::util::Vector;

namespace {

constexpr const char *kObjects = "objects";
constexpr const char *kSelected = "selected";
constexpr const char *kActive = "active";
constexpr const char *kFrame = "frame";
constexpr const char *kStartFrame = "startFrame";
constexpr const char *kFrameRate = "frameRate";

struct UnitField {
  const char *name;
  UnitFamily family;
};
constexpr UnitField kUnits[] = {{"lengthUnit", UnitFamily::Length},
                                {"angleUnit", UnitFamily::Angle},
                                {"timeUnit", UnitFamily::Time},
                                {"massUnit", UnitFamily::Mass},
                                {"factorUnit", UnitFamily::Factor}};

Vector<RefCell> withoutNulls(const Vector<RefCell> &cells)
{
  Vector<RefCell> out;
  for (const RefCell &c : cells) {
    if (!c.isNull()) {
      out.append(c);
    }
  }
  return out;
}

BlockEntry cellsField(const char *name, const Vector<RefCell> &cells)
{
  const Vector<RefCell> some = withoutNulls(cells);
  return detail::refField(name,
                          std::span<const RefCell>(some.data(), size_t(some.size())));
}

/* Those that aren't null, once each. A save writes null for a deleted block. */
bool takeCells(const Library &lib,
               const BlockEntry &e,
               const char *name,
               RefKind kind,
               Vector<RefCell> &out)
{
  if (!detail::isRefField(e, name)) {
    return false;
  }
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  litestl::util::Map<BlockId, int> seen;
  out.clear();
  for (const RefCell &c : e.refs) {
    if (!c.isNull() && seen.add(clients.canonical(c.target), 0)) {
      RefCell cell(kind);
      cell.target = c.target;
      out.append(cell);
    }
  }
  return true;
}

/* A number that isn't finite is dropped. */
bool takeNumber(const BlockEntry &e, const char *name, double &out)
{
  if (!detail::isValueField(e, name, PropValue::Type::Float)) {
    return false;
  }
  out = std::isfinite(e.value.asFloat()) ? e.value.asFloat() : out;
  return true;
}

bool takeRate(const BlockEntry &e, const char *name, FrameRate &out)
{
  if (!detail::isValueField(e, name, PropValue::Type::Vec2)) {
    return false;
  }
  const double num = e.value.component(0), den = e.value.component(1);
  const auto inRange = [](double v) { return v >= 1.0 && v <= FrameRate::kMax; };
  const Result<FrameRate> r =
      inRange(num) && inRange(den)
          ? FrameRate::of(int64_t(num), int64_t(den))
          : Result<FrameRate>(Status::error(StatusCode::OutOfRange, ""));
  if (!r.ok() || r->num() != num || r->den() != den) {
    return false;
  }
  out = *r;
  return true;
}

bool takeUnit(const BlockEntry &e, UnitSettings &units)
{
  for (const UnitField &f : kUnits) {
    if (detail::isValueField(e, f.name, PropValue::Type::String)) {
      return units.setDisplayUnit(f.family, e.value.asString().c_str()).ok();
    }
  }
  return false;
}

} // namespace

void Scene::write(BlockWriter &out) const
{
  const UnitSettings defaults;
  Vector<BlockEntry> fields = fields_;
  /* A setting whose value this build couldn't read is as it was, until it is set to
   * another. */
  const auto setting = [&](const char *name, PropValue v, bool isDefault) {
    if (!isDefault || !detail::hasField(fields_, name)) {
      fields.append(detail::valueField(name, std::move(v)));
    }
  };
  fields.append(cellsField(kObjects, cells_));
  fields.append(detail::refField(kActive, std::span<const RefCell>(&active_.cell(), 1)));
  fields.append(cellsField(kSelected, selection_));
  setting(kFrame, PropValue::ofFloat(frame), frame == 0.0);
  setting(kStartFrame, PropValue::ofFloat(units.startFrame), units.startFrame == 0.0);
  const double rate[] = {double(units.rate.num()), double(units.rate.den())};
  setting(kFrameRate,
          PropValue::ofComponents(PropValue::Type::Vec2, rate),
          units.rate == defaults.rate);
  for (const UnitField &f : kUnits) {
    setting(f.name,
            PropValue::ofString(units.displayUnit(f.family).symbol),
            units.displayIndex(f.family) == defaults.displayIndex(f.family));
  }
  out.entries(std::span<const BlockEntry>(fields.data(), size_t(fields.size())));
}

void Scene::readFrom(BlockReader &in)
{
  const Library &lib = in.library();
  detail::readFields(in, fields_, [&](const BlockEntry &e) {
    return takeCells(lib, e, kObjects, RefKind::OptStrong, cells_) ||
           takeCells(lib, e, kSelected, RefKind::Weak, selection_) ||
           detail::takeRef(e, kActive, active_.cell()) || takeNumber(e, kFrame, frame) ||
           takeNumber(e, kStartFrame, units.startFrame) ||
           takeRate(e, kFrameRate, units.rate) || takeUnit(e, units);
  });
}

Scene *Scene::duplicate() const
{
  Scene *s = litestl::alloc::New<Scene>("coredcc::Scene");
  s->cells_ = cells_;
  s->selection_ = selection_;
  s->active_ = active_;
  s->frame = frame;
  s->units = units;
  s->fields_ = fields_;
  return s;
}

uint64_t Scene::memSize() const
{
  return sizeof(Scene) + (uint64_t(cells_.size()) + selection_.size()) * sizeof(RefCell) +
         detail::fieldsMemSize(fields_);
}

} // namespace coredcc

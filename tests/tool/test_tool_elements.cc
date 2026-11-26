/* core.element.applyDelta and dragDelta on a points block: weights, the object's
 * matrix, symmetry partners and their conflicts, points on mirror planes, undo and redo,
 * a topoStamp that changed, invalidateData; and the transform tool's ELEMENTS target. */
#include "tool_fixture.h"

#include "coredcc/tool/elements.h"

#include <cmath>
#include <vector>

test_init;

using namespace tool_test;

namespace {

/* Points with a selection, weights and mirror partners found from where they start. */
struct Points {
  std::vector<double3> pos;
  std::vector<uint8_t> selected;
  std::vector<double> weight;
  std::vector<int32_t> mirror[3];
  double stamp = 1.0;
  uint32_t symAxes = 0;
  uint32_t invalidated = 0;
  int writes = 0;

  void build()
  {
    selected.assign(pos.size(), 0);
    weight.assign(pos.size(), 1.0);
    for (int axis = 0; axis < 3; axis++) {
      mirror[axis].assign(pos.size(), -1);
      for (size_t i = 0; i < pos.size(); i++) {
        double3 m = pos[i];
        m[axis] = -m[axis];
        for (size_t j = 0; j < pos.size(); j++) {
          if ((pos[j] - m).length() < 1e-12) {
            mirror[axis][i] = int32_t(j);
          }
        }
      }
    }
  }
  Box3d localBounds() const
  {
    Box3d b;
    for (const double3 &p : pos) {
      b.include(p[0], p[1], p[2]);
    }
    return b;
  }
  uint32_t domains() const
  {
    return 1;
  }
  void closestElements(const NearQuery &, NearSink &) const
  {
  }
  void invalidate(uint32_t kinds)
  {
    invalidated |= kinds;
  }
  double topoStamp() const
  {
    return stamp;
  }
  int32_t elementCount(int32_t) const
  {
    return int32_t(pos.size());
  }
  Vector<double> positions(const Vector<int32_t> &handles, int32_t) const
  {
    Vector<double> out;
    for (int32_t h : handles) {
      for (int k = 0; k < 3; k++) {
        out.append(pos[size_t(h)][k]);
      }
    }
    return out;
  }
  Status gather(int32_t domain, ElementSet &out) const
  {
    if (domain != int32_t(ElementDomain::Point)) {
      return Status();
    }
    for (size_t i = 0; i < pos.size(); i++) {
      if (selected[i]) {
        out.add(int32_t(i), pos[i][0], pos[i][1], pos[i][2], weight[i]);
      }
    }
    return Status();
  }
  Status setPositions(const Vector<int32_t> &handles, const Vector<double> &p)
  {
    writes++;
    for (int i = 0; i < int(handles.size()); i++) {
      pos[size_t(handles[i])] = double3(p[3 * i], p[3 * i + 1], p[3 * i + 2]);
    }
    return Status();
  }
  uint32_t axes() const
  {
    return symAxes;
  }
  Vector<int32_t> partners(const Vector<int32_t> &handles, int32_t axis) const
  {
    Vector<int32_t> out;
    for (int32_t h : handles) {
      out.append(mirror[axis][size_t(h)]);
    }
    return out;
  }
  static lbt::Struct<Points> *defineBindings()
  {
    auto *st = new lbt::Struct<Points>("test::Points", sizeof(Points));
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    BIND_STRUCT_METHOD(st, invalidate, MARGS("kinds"));
    BIND_STRUCT_METHOD(st, topoStamp, MARGS());
    BIND_STRUCT_METHOD(st, elementCount, MARGS("domain"));
    BIND_STRUCT_METHOD(st, positions, MARGS("handles", "domain"));
    BIND_STRUCT_METHOD(st, gather, MARGS("domain", "out"));
    BIND_STRUCT_METHOD(st, setPositions, MARGS("handles", "positions"));
    BIND_STRUCT_METHOD(st, axes, MARGS());
    BIND_STRUCT_METHOD(st, partners, MARGS("handles", "axis"));
    return st;
  }
};

struct ElementsDoc : ToolDoc {
  ElementsDoc()
  {
    addSlotType("test.Points");
    must(registerImpl<Spatial, Points>(rt, "test.Points"));
    must(registerImpl<Invalidation, Points>(rt, "test.Points"));
    must(registerImpl<ElementAccess, Points>(rt, "test.Points"));
    must(registerImpl<TransformableElements, Points>(rt, "test.Points"));
    must(registerImpl<Symmetry, Points>(rt, "test.Points"));
    ApiStruct st("test.Points");
    st.parent(StructId::of(kObjectDataStruct));
    must(rt.ext<ApiRegistry>().add(std::move(st)).status());
  }
  /* An object at `at` whose data is a Points block with `pts`. */
  BlockHandle pointsObject(const char *name, std::vector<double3> pts, double x = 0)
  {
    const BlockHandle h = object(name, x);
    const BlockHandle data =
        addSlot<Points>("test.Points", (std::string(name) + ".pts").c_str());
    points(data).pos = std::move(pts);
    points(data).build();
    setData(h, data);
    blocks.push_back(data);
    return h;
  }
  Points &points(BlockHandle data)
  {
    return *static_cast<Points *>(slot(data).data);
  }
  Points &pointsOf(int i)
  {
    return points(blocks[size_t(i)]);
  }
  PropertyBag apply(Inputs in)
  {
    return ok(kApplyElementDeltaCommand, in);
  }
  std::vector<BlockHandle> blocks;
};

bool near(const double3 &a, const double3 &b, double eps = 1e-12)
{
  return (a - b).length() <= eps;
}

void testWeightsAndUndo()
{
  ElementsDoc d;
  const BlockHandle o = d.pointsObject("o", {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, 10.0);
  Points &p = d.pointsOf(0);
  p.selected = {1, 1, 0};
  p.weight = {1.0, 0.5, 1.0};
  d.select({o});
  d.evaluate();
  PropertyBag out = d.apply({{"vector", PropValue::ofVec3(0, 0, 2)}});
  test_assert(out.get("points")->asInt() == 2 && out.get("skipped")->asInt() == 0);
  test_assert(near(p.pos[0], double3(1, 0, 2)) && near(p.pos[1], double3(0, 1, 1)));
  test_assert(near(p.pos[2], double3(0, 0, 1)));
  test_assert((p.invalidated & uint32_t(InvalidateKinds::Positions)) != 0);
  test_assert(d.history->size() == 1);
  must(d.ex->undo(*d.ctx));
  test_assert(near(p.pos[0], double3(1, 0, 0)) && near(p.pos[1], double3(0, 1, 0)));
  must(d.ex->redo(*d.ctx));
  test_assert(near(p.pos[0], double3(1, 0, 2)) && near(p.pos[1], double3(0, 1, 1)));

  /* A changed topoStamp: undo fails and writes nothing. */
  p.stamp = 2.0;
  const int writes = p.writes;
  test_assert(!d.ex->undo(*d.ctx).ok());
  d.rt.takeStickyError().ignore();
  test_assert(p.writes == writes && near(p.pos[0], double3(1, 0, 2)));
}

void testObjectMatrix()
{
  ElementsDoc d;
  const BlockHandle o = d.pointsObject("o", {{1, 0, 0}, {0, 2, 0}});
  d.pointsOf(0).selected = {1, 1};
  /* Scale the object by 2 and turn it, so that W isn't a translation. */
  d.select({o});
  d.ok(kApplyObjectDeltaCommand,
       {{"kind", PropValue::ofEnum(2)}, {"vector", PropValue::ofVec3(2, 2, 2)}});
  d.ok(kApplyObjectDeltaCommand,
       {{"kind", PropValue::ofEnum(1)},
        {"axis", PropValue::ofVec3(0, 0, 1)},
        {"angle", PropValue::ofFloat(0.5)}});
  d.evaluate();
  const mat4d w = d.result(o).state.world;
  const std::vector<double3> before = d.pointsOf(0).pos;
  /* A rotation about a world axis through a world point. */
  TransformDelta t;
  t.kind = DeltaKind::Rotate;
  t.axis = double3(1, 0, 0);
  t.angle = 0.7;
  t.pivot = double3(0.5, 0.5, 0.5);
  d.apply({{"kind", PropValue::ofEnum(1)},
           {"axis", PropValue::ofVec3(1, 0, 0)},
           {"angle", PropValue::ofFloat(0.7)},
           {"pivot", PropValue::ofVec3(0.5, 0.5, 0.5)}});
  for (size_t i = 0; i < before.size(); i++) {
    const double3 want = litestl::math::transformPoint(
        t.matrix(), litestl::math::transformPoint(w, before[i]));
    const double3 got = litestl::math::transformPoint(w, d.pointsOf(0).pos[i]);
    test_assert(near(got, want, 1e-12));
  }
  /* A weighted scale: 1 + w (s - 1). */
  d.pointsOf(0).weight = {0.5, 0.0};
  const std::vector<double3> b2 = d.pointsOf(0).pos;
  d.apply({{"kind", PropValue::ofEnum(2)},
           {"vector", PropValue::ofVec3(3, 3, 3)},
           {"pivot", PropValue::ofVec3(0, 0, 0)}});
  const double3 w0 = litestl::math::transformPoint(w, b2[0]);
  test_assert(
      near(litestl::math::transformPoint(w, d.pointsOf(0).pos[0]), w0 * 2.0, 1e-12));
  test_assert(near(d.pointsOf(0).pos[1], b2[1]));
}

void testSymmetry()
{
  ElementsDoc d;
  /* P, Q, S, R round the origin, and M on the x = 0 plane. */
  const BlockHandle o =
      d.pointsObject("o", {{1, 1, 0}, {-1, 1, 0}, {-1, -1, 0}, {1, -1, 0}, {0, 2, 0}});
  Points &p = d.pointsOf(0);
  p.symAxes = 1 | 2;
  p.selected = {1, 1, 0, 0, 0};
  d.select({o});
  d.evaluate();
  /* Without symmetry only the selected move. */
  d.apply({{"vector", PropValue::ofVec3(0.5, 0.25, 0)}});
  test_assert(near(p.pos[2], double3(-1, -1, 0)) && near(p.pos[3], double3(1, -1, 0)));
  must(d.ex->undo(*d.ctx));
  /* With it: R is P's partner across y; S is Q's across y, and P's across {x, y}: the
   * smaller set wins. */
  PropertyBag out = d.apply({{"vector", PropValue::ofVec3(0.5, 0.25, 0)},
                             {"symmetry", PropValue::ofBool(true)}});
  test_assert(near(p.pos[0], double3(1.5, 1.25, 0)) &&
              near(p.pos[1], double3(-0.5, 1.25, 0)));
  test_assert(near(p.pos[3], double3(1.5, -1.25, 0)));
  test_assert(near(p.pos[2], double3(-0.5, -1.25, 0)));
  test_assert(near(p.pos[4], double3(0, 2, 0)));
  /* Undo puts the partners back too. */
  must(d.ex->undo(*d.ctx));
  test_assert(near(p.pos[2], double3(-1, -1, 0)) && near(p.pos[3], double3(1, -1, 0)));
  /* A point on a mirror plane stays on it. */
  p.selected = {0, 0, 0, 0, 1};
  d.apply({{"vector", PropValue::ofVec3(0.5, 0.3, 0)},
           {"symmetry", PropValue::ofBool(true)}});
  test_assert(near(p.pos[4], double3(0, 2.3, 0)));
  /* The order gather gives them in doesn't matter: P alone moves R (across y) and Q
   * (across x), and S across both. */
  must(d.ex->undo(*d.ctx));
  p.selected = {1, 0, 0, 0, 0};
  d.apply({{"vector", PropValue::ofVec3(0.5, 0.25, 0)},
           {"symmetry", PropValue::ofBool(true)}});
  test_assert(near(p.pos[1], double3(-1.5, 1.25, 0)) &&
              near(p.pos[3], double3(1.5, -1.25, 0)));
  test_assert(near(p.pos[2], double3(-1.5, -1.25, 0)));
}

void testSkipsAndDetail()
{
  ElementsDoc d;
  const BlockHandle o = d.pointsObject("o", {{1, 0, 0}});
  const BlockHandle plain = d.box("plain", 3, 0, 0);
  d.pointsOf(0).selected = {1};
  d.select({o, plain});
  d.evaluate();
  PropertyBag out = d.apply({{"vector", PropValue::ofVec3(1, 0, 0)}});
  test_assert(out.get("skipped")->asInt() == 1 && out.get("points")->asInt() == 1);
  /* Nothing selected in the block: nothing to do. */
  d.pointsOf(0).selected = {0};
  test_assert(d.fails(kApplyElementDeltaCommand).code() ==
              StatusCode::FailedPrecondition);
}

void testToolDrag()
{
  ElementsDoc d;
  const BlockHandle o = d.pointsObject("o", {{1, 0, 0}, {-1, 0, 0}, {0, 3, 0}}, 2.0);
  Points &p = d.pointsOf(0);
  p.selected = {1, 1, 0};
  d.select({o});
  d.settingKey("target", "ELEMENTS");
  d.useTool(kTransformTool);
  /* The gizmo is at the selected points' mean, in the world. */
  const Frame f = d.gizmoFrame();
  test_assert(f.onScreen && near(f.pivot, double3(2, 0, 0)));
  const Pixel press = d.pixelOf(f.pivot + f.axes[0] * (f.unit * 0.7));
  const Pixel tip = d.pixelOf(f.pivot + f.axes[0] * f.unit);
  const Pixel to{press.x + (tip.x - f.centre.x) * 0.5,
                 press.y + (tip.y - f.centre.y) * 0.5};
  const double t0 = *axisParameter(d.vm, press.x, press.y, f.pivot, f.axes[0]);
  const double t1 = *axisParameter(d.vm, to.x, to.y, f.pivot, f.axes[0]);
  must(d.send(EventType::PointerDown, press.x, press.y).status());
  must(d.send(EventType::PointerMove, to.x, to.y).status());
  must(d.send(EventType::PointerUp, to.x, to.y).status());
  test_assert(d.history->size() == 1);
  test_assert(std::abs(p.pos[0][0] - (1.0 + (t1 - t0))) < 1e-12);
  test_assert(std::abs(p.pos[1][0] - (-1.0 + (t1 - t0))) < 1e-12 && p.pos[2][0] == 0.0);
  test_assert(d.isAt(o, 2.0)); /* the object stays */
  must(d.ex->undo(*d.ctx));
  test_assert(p.pos[0][0] == 1.0 && p.pos[1][0] == -1.0);
  /* A click on the gizmo makes no step. */
  d.evaluate();
  must(d.send(EventType::PointerDown, press.x, press.y).status());
  must(d.send(EventType::PointerUp, press.x, press.y).status());
  test_assert(d.history->size() == 1 && d.history->canRedo());
}

/* From the tools-and-overlay review: a click moves no point on a mirror plane, and a
 * delta that isn't finite, or gives points that aren't, is refused before any write. */
void testReviewFixes()
{
  ElementsDoc d;
  const BlockHandle o = d.pointsObject("o", {{1, 0, 0}, {0, 1, 0}, {1e-13, 2, 0}});
  Points &p = d.pointsOf(0);
  p.symAxes = 1;
  p.selected = {1, 1, 1};
  d.select({o});
  d.evaluate();
  const CommandDef *def = d.rt.ext<CommandRegistry>().find(kDragElementDeltaCommand);
  PropertyBag in = def->newInputs();
  must(in.set("symmetry", PropValue::ofBool(true)));
  must(d.ex->beginInteractive(*d.ctx, *def, std::move(in)));
  test_assert(!d.ex->interactiveCommand()->as<ElementsDelta>()->changed());
  test_assert(p.pos[2][0] == 1e-13); /* not snapped to 0 */
  const int writes = p.writes;
  const auto update = [&](const TransformDelta &t) {
    return d.ex->updateInteractive(*d.ctx, [&](Context &c, Command &cmd) {
      return cmd.as<ElementsDelta>()->update(c, t);
    });
  };
  TransformDelta nan;
  nan.vector = double3(std::nan(""), 0, 0);
  test_assert(update(nan).code() == StatusCode::InvalidArgument);
  TransformDelta zeroAxis;
  zeroAxis.kind = DeltaKind::Rotate;
  zeroAxis.axis = double3(0, 0, 0);
  zeroAxis.angle = 0.5;
  test_assert(update(zeroAxis).code() == StatusCode::InvalidArgument);
  test_assert(p.writes == writes && p.pos[0][0] == 1.0 && std::isfinite(p.pos[1][1]));
  if (d.ex->interactiveOpen()) {
    must(d.ex->cancel(*d.ctx));
  }
}

} // namespace

int main()
{
  testWeightsAndUndo();
  testObjectMatrix();
  testSymmetry();
  testSkipsAndDetail();
  testToolDrag();
  testReviewFixes();
  return test_end();
}

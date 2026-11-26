/* StrokeSampler against sculptcore's driver (stroke_golden.inc), its properties, and
 * core.stroke.apply and core.tool.stroke with a recording brush: replay, mirrors,
 * methods. */
#include "tool_fixture.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/tool/elements.h"
#include "coredcc/tool/stroke.h"

#include "litestl/binding/binding.h"

#include <cmath>
#include <string>
#include <vector>

using namespace tool_test;

test_init;

namespace {

struct GoldenInput {
  int stroke, batch;
  double x, y, pressure;
};
struct GoldenStroke {
  double radius, spacing;
  int batches;
};
struct GoldenSample {
  int stroke, batch;
  double x, y, pressure, angle;
  int interpolated;
};
#include "stroke_golden.inc"

/* What the recording brush saw, in order. */
struct Call {
  char kind; /* 'b'egin, 'a'pply, 'u'ndo */
  StrokeSample s;
  std::string object;
};
std::vector<Call> gCalls;

struct Recorder {
  Status begin(Context &, const StrokeTarget &t)
  {
    gCalls.push_back({'b', StrokeSample(), t.object.c_str()});
    return Status();
  }
  Status apply(Context &, const StrokeSample &s)
  {
    gCalls.push_back({'a', s, ""});
    return Status();
  }
  void undo(Context &)
  {
    gCalls.push_back({'u', StrokeSample(), ""});
  }
  uint64_t byteSize() const
  {
    return 100;
  }
  static lbt::Struct<Recorder> *defineBindings()
  {
    auto *st = new lbt::Struct<Recorder>("test::Recorder", sizeof(Recorder));
    BIND_STRUCT_METHOD(st, begin, MARGS("ctx", "target"));
    BIND_STRUCT_METHOD(st, apply, MARGS("ctx", "s"));
    BIND_STRUCT_METHOD(st, undo, MARGS("ctx"));
    BIND_STRUCT_METHOD(st, byteSize, MARGS());
    return st;
  }
};

/* A box of half-size `half` whose Spatial is its bounds, with Symmetry. */
struct MirrorBox {
  double half = 1.0;
  uint32_t symAxes = 0;

  Box3d localBounds() const
  {
    Box3d b;
    b.include(-half, -half, -half);
    b.include(half, half, half);
    return b;
  }
  uint32_t domains() const
  {
    return 0;
  }
  void closestElements(const NearQuery &, NearSink &) const
  {
  }
  uint32_t axes() const
  {
    return symAxes;
  }
  Vector<int32_t> partners(const Vector<int32_t> &handles, int32_t) const
  {
    return handles;
  }
  static lbt::Struct<MirrorBox> *defineBindings()
  {
    auto *st = new lbt::Struct<MirrorBox>("test::MirrorBox", sizeof(MirrorBox));
    BIND_STRUCT_METHOD(st, localBounds, MARGS());
    BIND_STRUCT_METHOD(st, domains, MARGS());
    BIND_STRUCT_METHOD(st, closestElements, MARGS("query", "sink"));
    BIND_STRUCT_METHOD(st, axes, MARGS());
    BIND_STRUCT_METHOD(st, partners, MARGS("handles", "axis"));
    return st;
  }
};

struct StrokeDoc : ToolDoc {
  StrokeDoc()
  {
    must(addNativeStrokeBrush<Recorder>(rt, "test.recorder"));
    test_assert(addNativeStrokeBrush<Recorder>(rt, "test.recorder").code() ==
                StatusCode::AlreadyExists);
    addSlotType("test.MirrorBox");
    must(registerImpl<Spatial, MirrorBox>(rt, "test.MirrorBox"));
    must(registerImpl<Symmetry, MirrorBox>(rt, "test.MirrorBox"));
    ApiStruct st("test.MirrorBox");
    st.parent(StructId::of(kObjectDataStruct));
    must(rt.ext<ApiRegistry>().add(std::move(st)).status());
    gCalls.clear();
    set("brush", PropValue::ofString("test.recorder"));
  }
  /* An object at `at` whose data is a MirrorBox mirrored across `axes`. */
  BlockHandle mirrorBox(const char *name, double x, uint32_t axes)
  {
    const BlockHandle h = object(name, x);
    const BlockHandle data = addSlot<MirrorBox>("test.MirrorBox", (std::string(name) + ".box").c_str());
    static_cast<MirrorBox *>(slot(data).data)->symAxes = axes;
    setData(h, data);
    return h;
  }
  void set(const char *name, const PropValue &v)
  {
    const ToolDef *def = rt.ext<ToolRegistry>().find(kStrokeTool);
    must(rt.ext<ToolSettingsStore>().set(*def, name, v));
  }
  std::vector<StrokeSample> applied() const
  {
    std::vector<StrokeSample> out;
    for (const Call &c : gCalls) {
      if (c.kind == 'a') {
        out.push_back(c.s);
      }
    }
    return out;
  }
  /* A press, moves in `steps` to `to`, and a release. */
  void drag(Pixel from, Pixel to, int steps)
  {
    must(send(EventType::PointerDown, from.x, from.y).status());
    for (int i = 1; i <= steps; i++) {
      const double t = double(i) / steps;
      must(send(EventType::PointerMove, from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t)
               .status());
    }
    must(send(EventType::PointerUp, to.x, to.y).status());
  }
};

StrokeSampler sampler(const ViewMath &vm, double radius, double spacing, int method = 0, int space = 0)
{
  StrokeSettings s;
  s.radius = radius;
  s.spacing = spacing;
  s.method = StrokeMethod(method);
  s.space = StrokeSpace(space);
  StrokeSampler out;
  must(out.configure(s));
  out.setView(vm);
  return out;
}

PointerSample at(double x, double y, double pressure = 0.5)
{
  PointerSample p;
  p.x = x;
  p.y = y;
  p.pressure = pressure;
  return p;
}

bool near(double a, double b, double eps)
{
  return std::fabs(a - b) <= eps;
}

bool same(const double3 &a, const double3 &b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

} // namespace

/* The same samples as sculptcore's driver gives, from float positions (1e-3 px). */
void testGoldens()
{
  ToolDoc d;
  const int nStrokes = int(sizeof(kGoldenStrokes) / sizeof(kGoldenStrokes[0]));
  size_t g = 0;
  const size_t nSamples = sizeof(kGoldenSamples) / sizeof(kGoldenSamples[0]);
  for (int s = 0; s < nStrokes; s++) {
    StrokeSampler sm = sampler(d.vm, kGoldenStrokes[s].radius, kGoldenStrokes[s].spacing);
    for (int b = 0; b <= kGoldenStrokes[s].batches; b++) {
      for (const GoldenInput &in : kGoldenInputs) {
        if (in.stroke == s && in.batch == b) {
          sm.push(at(in.x, in.y, in.pressure));
        }
      }
      if (b == kGoldenStrokes[s].batches) {
        sm.end();
      }
      const int32_t n = sm.poll();
      for (int32_t i = 0; i < n; i++, g++) {
        test_assert(g < nSamples);
        const GoldenSample &want = kGoldenSamples[g];
        const StrokeSample got = sm.sample(i);
        test_assert(want.stroke == s && want.batch == b);
        test_assert(near(got.x, want.x, 1e-3) && near(got.y, want.y, 1e-3));
        test_assert(near(got.pressure, want.pressure, 1e-6));
        test_assert(near(got.angle, want.angle, 1e-4));
        test_assert(got.interpolated == (want.interpolated != 0));
      }
    }
    test_assert(sm.finished() && sm.poll() == 0);
  }
  test_assert(g == nSamples);
}

/* Along a line of evenly spaced points: the spacing (within 1e-9 of the step), the ends, and
 * the pressure (linear, since the spline is). */
void testLine()
{
  ToolDoc d;
  StrokeSampler sm = sampler(d.vm, 10.0, 0.25); /* a step of 5 px */
  const int n = 12;
  for (int i = 0; i < n; i++) {
    sm.push(at(100 + 7.5 * i, 300, double(i) / (n - 1)));
  }
  sm.end();
  const int32_t count = sm.poll();
  const double length = 7.5 * (n - 1);
  test_assert(count == int32_t(std::floor(length / 5.0)) + 1);
  const StrokeSample first = sm.sample(0);
  test_assert(first.x == 100 && first.y == 300 && !first.interpolated && first.distance == 0);
  for (int32_t i = 1; i < count; i++) {
    const StrokeSample a = sm.sample(i - 1), b = sm.sample(i);
    test_assert(b.interpolated && near(b.step, 5.0, 1e-12));
    test_assert(near(std::hypot(b.x - a.x, b.y - a.y), 5.0, 1e-9));
    test_assert(near(b.distance, 5.0 * i, 1e-9) && near(b.angle, 0.0, 1e-12));
    test_assert(near(b.pressure, (b.x - 100) / length, 1e-9));
  }
  const StrokeSample last = sm.sample(count - 1);
  test_assert(100 + length - last.x < 5.0);
  test_assert(last.radiusPixels == 10.0 && last.radiusWorld > 0.0);

  /* A spacing walk in pieces: the same samples as at once. */
  StrokeSampler pieces = sampler(d.vm, 10.0, 0.25);
  std::vector<StrokeSample> got;
  for (int i = 0; i < n; i++) {
    pieces.push(at(100 + 7.5 * i, 300, double(i) / (n - 1)));
    for (int32_t k = 0, m = pieces.poll(); k < m; k++) {
      got.push_back(pieces.sample(k));
    }
  }
  pieces.end();
  for (int32_t k = 0, m = pieces.poll(); k < m; k++) {
    got.push_back(pieces.sample(k));
  }
  test_assert(int32_t(got.size()) == count);
  for (int32_t i = 0; i < count; i++) {
    test_assert(near(got[size_t(i)].x, sm.sample(i).x, 1e-9));
  }

  /* Bad settings. */
  StrokeSettings bad;
  bad.radius = 0;
  test_assert(sm.configure(bad).code() == StatusCode::InvalidArgument);
  bad = StrokeSettings();
  bad.method = StrokeMethod(7);
  test_assert(sm.configure(bad).code() == StatusCode::InvalidArgument);
}

/* core.tool.stroke over a box: one step, the brush sees the samples, undo and redo replays
 * them exactly. */
void testReplay()
{
  StrokeDoc d;
  const BlockHandle b = d.box("b", 0, 0, 0, 1.0);
  d.select({b});
  d.useTool(kStrokeTool);
  const Pixel c = d.pixelOf(double3(0, 0, 0));
  d.drag(Pixel{c.x - 40, c.y}, Pixel{c.x + 40, c.y}, 8);
  test_assert(d.history->size() == 1 && !d.ex->interactiveOpen());
  test_assert(gCalls.size() > 2 && gCalls[0].kind == 'b');
  test_assert(gCalls[0].object == formatRef(d.rt, *d.ptr(b).tryPointer()));
  const std::vector<StrokeSample> first = d.applied();
  test_assert(first.size() >= 16); /* 80 px at a step of 5 */
  int hits = 0;
  for (const StrokeSample &s : first) {
    hits += s.hit ? 1 : 0;
    test_assert(s.mirror == 0);
  }
  test_assert(hits == int(first.size())); /* the box covers the whole drag */
  const StrokeSample &mid = first[first.size() / 2];
  test_assert(std::fabs(mid.normal.length() - 1.0) < 1e-9);

  gCalls.clear();
  d.undo();
  test_assert(gCalls.size() == 1 && gCalls[0].kind == 'u');
  gCalls.clear();
  d.redo();
  test_assert(gCalls[0].kind == 'b');
  const std::vector<StrokeSample> again = d.applied();
  test_assert(again.size() == first.size());
  for (size_t i = 0; i < first.size(); i++) {
    test_assert(again[i].x == first[i].x && again[i].y == first[i].y);
    test_assert(same(again[i].world, first[i].world) && again[i].pressure == first[i].pressure);
  }
  /* head() is null after an undo: the record by its place. */
  test_assert(d.history->at(0).byteSize() >= 100 + first.size() * sizeof(StrokeSample));

  /* Escape cancels: the brush undoes, nothing is recorded. */
  gCalls.clear();
  must(d.send(EventType::PointerDown, c.x, c.y).status());
  must(d.send(EventType::PointerMove, c.x + 20, c.y).status());
  InputEvent esc;
  esc.type = EventType::KeyDown;
  esc.key = keys::kEscape;
  must(d.send(esc).status());
  test_assert(d.history->size() == 1 && !d.ex->interactiveOpen());
  test_assert(gCalls.back().kind == 'u');
}

/* Each sample once more per mirror, reflected in the object's space. */
void testMirror()
{
  StrokeDoc d;
  const BlockHandle b = d.mirrorBox("b", 2, 1 | 4);
  d.select({b});
  d.set("symmetry", PropValue::ofBool(true));
  d.useTool(kStrokeTool);
  const Pixel c = d.pixelOf(double3(2.3, 0, 0.2));
  d.drag(c, Pixel{c.x + 20, c.y + 5}, 3);
  const std::vector<StrokeSample> s = d.applied();
  test_assert(s.size() >= 8 && s.size() % 4 == 0);
  for (size_t i = 0; i < s.size(); i += 4) {
    test_assert(s[i].mirror == 0 && s[i + 1].mirror == 1 && s[i + 2].mirror == 4 &&
                s[i + 3].mirror == 5);
    const double3 p = s[i].world;
    test_assert((s[i + 1].world - double3(4 - p[0], p[1], p[2])).length() < 1e-9);
    test_assert((s[i + 2].world - double3(p[0], p[1], -p[2])).length() < 1e-9);
    test_assert((s[i + 3].world - double3(4 - p[0], p[1], -p[2])).length() < 1e-9);
    const double3 n = s[i].normal;
    test_assert((s[i + 3].normal - double3(-n[0], n[1], -n[2])).length() < 1e-9);
    test_assert(s[i + 1].x == s[i].x); /* the pixel is the stroke's */
  }
  /* Without the setting, no mirrors. */
  gCalls.clear();
  d.set("symmetry", PropValue::ofBool(false));
  d.drag(c, Pixel{c.x + 20, c.y + 5}, 3);
  for (const StrokeSample &x : d.applied()) {
    test_assert(x.mirror == 0);
  }
}

/* DragDot: one sample per pointer sample, coalesced ones too. Anchored: at the anchor, with
 * the drag; off the object it makes no stroke. World spacing. */
void testMethods()
{
  StrokeDoc d;
  const BlockHandle b = d.box("b", 0, 0, 0, 1.0);
  d.select({b});
  d.set("method", PropValue::ofEnum(int64_t(StrokeMethod::DragDot)));
  d.useTool(kStrokeTool);
  const Pixel c = d.pixelOf(double3(0, 0, 0));
  must(d.send(EventType::PointerDown, c.x, c.y).status());
  InputEvent move;
  move.type = EventType::PointerMove;
  move.x = c.x + 12;
  move.y = c.y;
  move.buttons = 1;
  for (int i = 1; i <= 3; i++) {
    move.addSample(at(c.x + 3 * i, c.y));
  }
  must(d.send(move).status());
  must(d.send(EventType::PointerUp, c.x + 12, c.y).status());
  std::vector<StrokeSample> s = d.applied();
  test_assert(s.size() == 5);
  test_assert(s[1].x == c.x + 3 && s[4].x == c.x + 12 && !s[4].interpolated);

  gCalls.clear();
  d.set("method", PropValue::ofEnum(int64_t(StrokeMethod::Anchored)));
  d.drag(c, Pixel{c.x + 30, c.y - 10}, 3);
  s = d.applied();
  test_assert(s.size() == 4);
  for (const StrokeSample &x : s) {
    test_assert(x.x == c.x && x.y == c.y && same(x.world, s[0].world) && x.hit);
  }
  test_assert(near(s[3].dragX, 30, 1e-12) && near(s[3].dragY, -10, 1e-12));
  test_assert(s[3].dragWorld.length() > 0.0 && s[0].dragWorld.length() == 0.0);

  const int steps = int(d.history->size());
  gCalls.clear();
  d.drag(Pixel{5, 5}, Pixel{20, 5}, 3); /* off the object */
  test_assert(d.applied().empty() && d.history->size() == steps);

  /* World spacing: 0.5 × 2 × 0.1 = a step of 0.1 on the box's face. */
  gCalls.clear();
  d.set("method", PropValue::ofEnum(int64_t(StrokeMethod::Path)));
  d.set("space", PropValue::ofEnum(int64_t(StrokeSpace::World)));
  d.set("radiusIsWorld", PropValue::ofBool(true));
  d.set("radius", PropValue::ofFloat(0.1));
  d.set("spacing", PropValue::ofFloat(0.5));
  d.lookFrom(double3(0, -8, 0));
  d.useTool(kStrokeTool);
  const Pixel l = d.pixelOf(double3(-0.6, -1, 0)), r = d.pixelOf(double3(0.6, -1, 0));
  d.drag(l, r, 6);
  s = d.applied();
  test_assert(s.size() >= 10);
  for (size_t i = 1; i < s.size(); i++) {
    test_assert(near((s[i].world - s[i - 1].world).length(), 0.1, 1e-6));
    test_assert(near(s[i].world[1], -1.0, 1e-9) && near(s[i].radiusWorld, 0.1, 1e-12));
  }
}

/* The stroke's command without a brush, and with one that isn't registered. */
void testErrors()
{
  StrokeDoc d;
  const BlockHandle b = d.box("b");
  d.select({b});
  d.set("brush", PropValue::ofString("test.recordr"));
  d.useTool(kStrokeTool);
  const Pixel c = d.pixelOf(double3(0, 0, 0));
  must(d.send(EventType::PointerDown, c.x, c.y).status());
  test_assert(!d.ex->interactiveOpen() && d.history->size() == 0);
  test_assert(std::string(d.tm.status().c_str()).find("no stroke brush 'test.recordr'") != std::string::npos);
  Result<Owned<StrokeBrush>> typo = d.rt.ext<StrokeBrushRegistry>().make(d.rt, "test.recordr");
  test_assert(typo.status().code() == StatusCode::NotFound);
  test_assert(typo.status().suggestions().size() == 1 &&
              std::string(typo.status().suggestions()[0].c_str()) == "test.recorder");
  must(d.send(EventType::PointerUp, c.x, c.y).status());

  /* No object. */
  d.select({});
  d.set("brush", PropValue::ofString("test.recorder"));
  must(d.send(EventType::PointerDown, c.x, c.y).status());
  test_assert(!d.ex->interactiveOpen() && std::string(d.tm.status().c_str()).find("no active object") != std::string::npos);
}

/* A press whose stroke can't begin, because the host has a drag open: the host's drag
 * stays (the tool cancelled it). */
void testHostDragKept()
{
  StrokeDoc d;
  const BlockHandle b = d.box("b");
  d.select({b});
  d.set("brush", PropValue::ofString("test.recorder"));
  d.useTool(kStrokeTool);
  const CommandDef *drag = d.rt.ext<CommandRegistry>().find(kDragObjectDeltaCommand);
  must(d.ex->beginInteractive(*d.ctx, *drag, drag->newInputs()));
  const uint64_t serial = d.ex->interactiveSerial();
  InputEvent e;
  e.type = EventType::PointerDown;
  e.x = 400;
  e.y = 300;
  must(d.tm.dispatch(*d.ctx, d.view, e).status());
  test_assert(d.ex->interactiveOpen() && d.ex->interactiveSerial() == serial);
  test_assert(std::string(d.tm.status().c_str()).rfind("Stroke: ", 0) == 0);
  must(d.ex->cancel(*d.ctx));
}

/* The smallest radius and spacing: half a device pixel apart, and at most
 * kMaxSamplesPerPoll from one poll (the review's 400 pixels gave 30,000,001). */
void testFlood()
{
  ToolDoc d;
  StrokeSampler sm;
  StrokeSettings st;
  st.radius = 1e-9;
  st.spacing = 1e-4;
  test_assert(sm.configure(st).code() == StatusCode::InvalidArgument);
  st.radius = StrokeSettings::kMinRadius;
  st.spacing = StrokeSettings::kMinSpacing;
  must(sm.configure(st));
  sm.setView(d.vm);
  int64_t total = 0;
  for (double x : {100.0, 200.0, 300.0, 400.0, 500.0}) {
    PointerSample p;
    p.x = x;
    p.y = 300;
    sm.push(p);
    total += sm.poll();
  }
  /* 300 pixels fixed (the last segment is held back), at 0.5: about 600. */
  test_assert(total > 550 && total < 650 && sm.dropped() == 0);
  PointerSample far;
  far.x = 3500;
  far.y = 300;
  sm.push(far);
  far.x = 3501;
  sm.push(far);
  test_assert(sm.poll() == StrokeSampler::kMaxSamplesPerPoll && sm.dropped() > 1000);
}

int main()
{
  testGoldens();
  testLine();
  testReplay();
  testMirror();
  testMethods();
  testErrors();
  testHostDragKept();
  testFlood();
  return test_end();
}

/* DrawList and DrawListReader: every record kind, precision far from the origin,
 * refusals, and a fuzz of altered buffers through the reader. */
#include "coredcc/draw/drawlist.h"

#include "litestl/binding/binding.h"
#include "litestl/math/matrix_bindings.h"

#include "fuzz.h"
#include "test_util.h"

#include <cmath>
#include <cstring>
#include <vector>

test_init;

using namespace coredcc;

namespace {

mat4d translation(double x, double y, double z)
{
  mat4d m;
  m.identity();
  m[3][0] = x;
  m[3][1] = y;
  m[3][2] = z;
  return m;
}

DrawStyle style(uint32_t id = 0, DepthMode depth = DepthMode::Test)
{
  DrawStyle s;
  s.color = 0x80402010u;
  s.width = 2.5f;
  s.depth = int32_t(depth);
  s.id = id;
  return s;
}

/* One record of each kind, under an origin. */
void fill(DrawList &d, const double3 &origin)
{
  test_assert(d.begin(origin).ok());
  const double3 seg[2] = {origin + double3(1, 0, 0), origin + double3(0, 2, 0)};
  test_assert(d.lines(style(1), seg).ok());
  test_assert(
      d.points(style(2, DepthMode::Always), std::span<const double3>(seg, 1)).ok());
  const double3 tri[3] = {origin, origin + double3(1, 0, 0), origin + double3(0, 1, 0)};
  const uint32_t colors[3] = {1, 2, 3};
  test_assert(d.triangles(style(3, DepthMode::XRay), tri, colors).ok());
  test_assert(
      d.shape(style(4), ShapeKind::Arrow, translation(origin[0], origin[1], origin[2]))
          .ok());
  test_assert(d.pushMatrix(translation(origin[0] + 5, origin[1], origin[2])).ok());
  const double3 local[2] = {double3(0, 0, 0), double3(0, 0, 1)};
  test_assert(d.lines(style(5), local).ok());
  test_assert(d.popMatrix().ok());
  test_assert(d.circle2d(style(6), 100, 50, 8, true).ok());
  const double xy[6] = {0, 0, 10, 0, 10, 10};
  test_assert(d.line2d(style(7), xy).ok());
  test_assert(d.end().ok());
}

void testRoundTrip()
{
  DrawList d;
  fill(d, double3(10, 20, 30));
  test_assert(d.frozen() && d.recordCount() == 9 && d.size() % 8 == 0);
  DrawListReader r;
  test_assert(r.read(d.bytes()).ok());
  test_assert(r.origin()[0] == 10 && r.origin()[2] == 30);
  const auto &it = r.items();
  test_assert(it.size() == 9);
  const DrawKind kinds[9] = {DrawKind::Lines,
                             DrawKind::Points,
                             DrawKind::Triangles,
                             DrawKind::Shape,
                             DrawKind::PushMatrix,
                             DrawKind::Lines,
                             DrawKind::PopMatrix,
                             DrawKind::Circle2D,
                             DrawKind::Line2D};
  for (int i = 0; i < 9; i++) {
    test_assert(it[i].kind == kinds[i] && it[i].offset % 8 == 0);
  }
  test_assert(it[0].aux == 2 && it[0].style.id == 1 && it[0].style.color == 0x80402010u &&
              it[0].style.width == 2.5f);
  /* Outside a push: relative to the origin. */
  test_assert(it[0].vertex(0, 0) == 1.0f && it[0].vertex(1, 1) == 2.0f);
  test_assert(it[1].style.depth == int32_t(DepthMode::Always));
  test_assert(it[2].flags == drawflags::kCornerColors && it[2].cornerColor(2) == 3 &&
              it[2].style.depth == int32_t(DepthMode::XRay));
  /* A shape outside a push: its translation minus the origin. */
  mat4d sm = it[3].matrix();
  test_assert(it[3].aux == uint32_t(ShapeKind::Arrow) && sm[3][0] == 0.0 &&
              sm[0][0] == 1.0);
  test_assert(it[4].matrix()[3][0] == 5.0);
  /* Inside a push: as given. */
  test_assert(it[5].vertex(1, 2) == 1.0f);
  test_assert(it[7].flags == drawflags::kFilled && it[7].vertex(0, 0) == 100.0f &&
              it[7].vertex(0, 2) == 8.0f);
  test_assert(it[8].aux == 3 && it[8].vertex(2, 1) == 10.0f);

  /* begin empties it again. */
  test_assert(d.begin(double3(0, 0, 0)).ok());
  test_assert(!d.frozen() && d.recordCount() == 0 && d.size() == DrawList::kHeaderBytes);
}

/* What is stored is the float of p - origin, computed in double. */
void testPrecision()
{
  for (double offset : {0.0, 1e3, 1e6, 1e9, -1e9}) {
    const double3 origin(offset, -offset, offset * 0.5);
    DrawList d;
    test_assert(d.begin(origin).ok());
    std::vector<double3> pts;
    coredcc_test::SplitMix64 rng(42);
    for (int i = 0; i < 64; i++) {
      const double dx = double(rng.next() >> 11) * 0x1p-53 * 200 - 100;
      pts.push_back(origin + double3(dx, dx * 0.37, -dx));
    }
    test_assert(d.points(style(), pts).ok());
    test_assert(d.end().ok());
    DrawListReader r;
    test_assert(r.read(d.bytes()).ok());
    for (int i = 0; i < 64; i++) {
      for (int a = 0; a < 3; a++) {
        test_assert(r.items()[0].vertex(uint32_t(i), a) == float(pts[i][a] - origin[a]));
      }
    }
    /* A shape far away keeps its translation exactly. */
    test_assert(d.begin(origin).ok());
    test_assert(d.shape(style(), ShapeKind::Cube, translation(offset + 0.25, 0, 0)).ok());
    test_assert(d.end().ok());
    test_assert(r.read(d.bytes()).ok());
    test_assert(r.items()[0].matrix()[3][0] == (offset + 0.25) - origin[0]);
  }
}

void testRefusals()
{
  DrawList d;
  const double nan = std::nan("");
  const double3 bad[2] = {double3(nan, 0, 0), double3(0, 0, 0)};
  test_assert(d.lines(style(), bad).code() == StatusCode::InvalidArgument);
  const double3 odd[3] = {double3(0, 0, 0), double3(1, 0, 0), double3(2, 0, 0)};
  test_assert(!d.lines(style(), odd).ok());
  test_assert(!d.triangles(style(), std::span<const double3>(odd, 2)).ok());
  const uint32_t one[1] = {0};
  test_assert(!d.triangles(style(), odd, one).ok());
  const double3 huge[1] = {double3(1e39, 0, 0)};
  test_assert(!d.points(style(), huge).ok());
  DrawStyle s = style();
  s.depth = 3;
  test_assert(d.points(style(), std::span<const double3>()).ok());
  test_assert(!d.points(s, std::span<const double3>(odd, 1)).ok());
  s = style();
  s.width = -1;
  test_assert(!d.circle2d(s, 0, 0, 1, false).ok());
  test_assert(!d.circle2d(style(), 0, 0, -1, false).ok());
  test_assert(!d.shape(style(), ShapeKind(99), translation(0, 0, 0)).ok());
  mat4d proj = translation(0, 0, 0);
  proj[2][3] = 1.0;
  test_assert(!d.shape(style(), ShapeKind::Cube, proj).ok());
  test_assert(!d.pushMatrix(proj).ok());
  test_assert(!d.popMatrix().ok());
  const double xy[3] = {0, 0, 1};
  test_assert(!d.line2d(style(), xy).ok());
  /* Nothing of that was appended. */
  test_assert(d.recordCount() == 1); /* the empty points record */

  /* end pops what is left, and says so. */
  test_assert(d.pushMatrix(translation(1, 2, 3)).ok());
  test_assert(d.end().code() == StatusCode::InvalidArgument && d.depth() == 0);
  DrawListReader r;
  test_assert(r.read(d.bytes()).ok());
  /* Frozen. */
  test_assert(!d.points(style(), std::span<const double3>(odd, 1)).ok());
  test_assert(d.end().ok());

  test_assert(d.begin(double3(nan, 0, 0)).code() == StatusCode::InvalidArgument);
  test_assert(d.origin()[0] == 0.0);
}

void testScripts()
{
  DrawList d;
  litestl::util::Vector<double> xyz = {0, 0, 0, 1, 1, 1};
  test_assert(d.linesXyz(style(), xyz).ok() && d.pointsXyz(style(), xyz).ok());
  xyz.append(2);
  test_assert(!d.linesXyz(style(), xyz).ok());
  litestl::util::Vector<double> tri = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  litestl::util::Vector<double> colors = {1, 2, 4294967295.0};
  test_assert(d.trianglesXyz(style(), tri, colors).ok());
  colors[0] = 0.5;
  test_assert(!d.trianglesXyz(style(), tri, colors).ok());
  test_assert(d.shapeOf(style(), int32_t(ShapeKind::Ring), translation(0, 0, 0)).ok());
  litestl::util::Vector<double> xy = {0, 0, 4, 4};
  test_assert(d.line2dXy(style(), xy).ok());
  test_assert(d.end().ok() && d.recordCount() == 5);
  std::vector<uint8_t> copy(size_t(d.size()));
  test_assert(d.read(0, copy).ok());
  test_assert(std::memcmp(copy.data(), d.bytes().data(), copy.size()) == 0);
  test_assert(d.read(d.size(), std::span<uint8_t>()).ok());
  test_assert(d.read(1, copy).code() == StatusCode::OutOfRange);
  test_assert(d.read(-1, std::span<uint8_t>()).code() == StatusCode::OutOfRange);
  test_assert(d.address() == int64_t(reinterpret_cast<intptr_t>(d.bytes().data())));
  test_assert(DrawList::defineBindings() == DrawList::defineBindings());
  test_assert(DrawStyle::defineBindings() == DrawStyle::defineBindings());
}

/* Truncated and altered buffers: refused or read, never read past the end. */
void testFuzz()
{
  DrawList d;
  fill(d, double3(1, 2, 3));
  const std::vector<uint8_t> good(d.bytes().begin(), d.bytes().end());
  DrawListReader r;
  for (size_t n = 0; n < good.size(); n++) {
    /* A copy of exactly n bytes, so a sanitizer sees a read past it. */
    std::vector<uint8_t> cut(good.begin(), good.begin() + std::ptrdiff_t(n));
    test_assert(!r.read(cut).ok());
  }
  coredcc_test::SplitMix64 rng(7);
  const int rounds = 20000 * coredcc_test::testScale();
  int accepted = 0;
  for (int i = 0; i < rounds; i++) {
    std::vector<uint8_t> b = good;
    const int edits = 1 + int(rng.below(4));
    for (int e = 0; e < edits; e++) {
      b[rng.below(uint32_t(b.size()))] = uint8_t(rng.next());
    }
    if (r.read(b).ok()) {
      accepted++;
      /* Whatever it accepted is consistent: items inside the bytes. */
      for (const DrawItem &item : r.items()) {
        test_assert(item.offset + DrawList::kRecordHeaderBytes + item.payloadBytes <=
                    b.size());
      }
    }
  }
  /* Colors, ids and some coordinates may change freely. */
  test_assert(accepted > 0 && accepted < rounds);
}

} // namespace

int main()
{
  testRoundTrip();
  testPrecision();
  testRefusals();
  testScripts();
  testFuzz();
  return test_end();
}

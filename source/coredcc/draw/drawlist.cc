#include "coredcc/draw/drawlist.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

namespace coredcc {

namespace {

uint64_t pad8(uint64_t n)
{
  return (n + 7) & ~uint64_t(7);
}

Status bad(const char *what, const std::string &why)
{
  return Status::error(StatusCode::InvalidArgument,
                       (std::string("DrawList::") + what + ": " + why).c_str());
}

/* Finite, and finite as a float too. */
bool fitsFloat(double v)
{
  return std::isfinite(v) && std::fabs(v) <= double(FLT_MAX);
}

bool finite3(const double3 &p)
{
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

bool affine(const double *d)
{
  return d[3] == 0.0 && d[7] == 0.0 && d[11] == 0.0 && d[15] == 1.0;
}

template <class T> void put(uint8_t *p, const T &v)
{
  std::memcpy(p, &v, sizeof(T));
}

template <class T> T get(const uint8_t *p)
{
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

Status checkStyle(const DrawStyle &s, const char *what)
{
  if (s.depth < 0 || s.depth > int32_t(DepthMode::XRay)) {
    return bad(what, "no such depth mode " + std::to_string(s.depth));
  }
  if (!std::isfinite(s.width) || s.width < 0.0f) {
    return bad(what, "the width isn't a number of 0 or more");
  }
  return Status();
}

} // namespace

/* ---- DrawList ---- */

DrawList::DrawList()
{
  begin(double3(0.0, 0.0, 0.0)).ignore();
}

Status DrawList::begin(const double3 &origin)
{
  const bool ok = finite3(origin);
  origin_ = ok ? origin : double3(0.0, 0.0, 0.0);
  words_.clear();
  words_.resize(kHeaderBytes / 8);
  size_ = kHeaderBytes;
  records_ = 0;
  depth_ = 0;
  frozen_ = false;
  finishHeader();
  return ok ? Status() : bad("begin", "the origin isn't finite");
}

Status DrawList::end()
{
  if (frozen_) {
    return Status();
  }
  const int32_t left = depth_;
  while (depth_ > 0) {
    popMatrix().ignore();
  }
  frozen_ = true;
  return left > 0 ? bad("end", std::to_string(left) + " pushes left, now popped")
                  : Status();
}

void DrawList::finishHeader()
{
  uint8_t *h = reinterpret_cast<uint8_t *>(words_.data());
  put<uint32_t>(h, kMagic);
  put<uint16_t>(h + 4, kVersion);
  put<uint16_t>(h + 6, 0);
  put<uint32_t>(h + 8, uint32_t(records_));
  put<uint32_t>(h + 12, uint32_t(size_));
  for (int i = 0; i < 3; i++) {
    put<double>(h + 16 + 8 * i, origin_[i]);
  }
  put<uint64_t>(h + 40, 0);
}

Status DrawList::room(uint64_t recordBytes, const char *what) const
{
  if (frozen_) {
    return bad(what, "the list is frozen until the next begin");
  }
  if (recordBytes > kMaxRecordBytes || size_ + recordBytes > uint64_t(UINT32_MAX)) {
    return bad(what, "too large for the format");
  }
  return Status();
}

uint8_t *DrawList::append(DrawKind kind,
                          const DrawStyle &style,
                          uint8_t flags,
                          uint32_t aux,
                          uint32_t payloadBytes)
{
  const uint64_t rec = kRecordHeaderBytes + pad8(payloadBytes);
  const uint64_t at = size_;
  words_.resize(size_t((at + rec) / 8));
  uint8_t *p = reinterpret_cast<uint8_t *>(words_.data()) + at;
  put<uint16_t>(p, uint16_t(kind));
  put<uint8_t>(p + 2, uint8_t(style.depth));
  put<uint8_t>(p + 3, flags);
  put<uint32_t>(p + 4, uint32_t(rec));
  put<uint32_t>(p + 8, style.color);
  put<float>(p + 12, style.width);
  put<uint32_t>(p + 16, style.id);
  put<uint32_t>(p + 20, aux);
  size_ += rec;
  records_++;
  finishHeader();
  return p + kRecordHeaderBytes;
}

Status DrawList::vertices(DrawKind kind,
                          const DrawStyle &style,
                          std::span<const double3> pts,
                          int multiple,
                          std::span<const uint32_t> colors,
                          const char *what)
{
  CDCC_TRY(checkStyle(style, what));
  if (pts.size() % size_t(multiple) != 0) {
    return bad(what,
               std::to_string(pts.size()) + " points, not a multiple of " +
                   std::to_string(multiple));
  }
  if (!colors.empty() && colors.size() != pts.size()) {
    return bad(what, "one color per corner, or none");
  }
  const uint64_t payload = uint64_t(pts.size()) * 12 + uint64_t(colors.size()) * 4;
  CDCC_TRY(room(kRecordHeaderBytes + pad8(payload), what));
  const double3 base = depth_ == 0 ? origin_ : double3(0.0, 0.0, 0.0);
  for (size_t i = 0; i < pts.size(); i++) {
    for (int a = 0; a < 3; a++) {
      if (!fitsFloat(pts[i][a] - base[a])) {
        return bad(what, "point " + std::to_string(i) + " isn't finite as a float");
      }
    }
  }
  uint8_t *p = append(kind,
                      style,
                      colors.empty() ? 0 : drawflags::kCornerColors,
                      uint32_t(pts.size()),
                      uint32_t(payload));
  for (size_t i = 0; i < pts.size(); i++) {
    for (int a = 0; a < 3; a++) {
      put<float>(p, float(pts[i][a] - base[a]));
      p += 4;
    }
  }
  for (uint32_t c : colors) {
    put<uint32_t>(p, c);
    p += 4;
  }
  return Status();
}

Status DrawList::lines(const DrawStyle &style, std::span<const double3> pairs)
{
  return vertices(DrawKind::Lines, style, pairs, 2, {}, "lines");
}

Status DrawList::points(const DrawStyle &style, std::span<const double3> pts)
{
  return vertices(DrawKind::Points, style, pts, 1, {}, "points");
}

Status DrawList::triangles(const DrawStyle &style,
                           std::span<const double3> corners,
                           std::span<const uint32_t> colors)
{
  return vertices(DrawKind::Triangles, style, corners, 3, colors, "triangles");
}

namespace {

/* A matrix as stored: outside any push, with its translation minus the origin. */
Status storedMatrix(const mat4d &m,
                    bool outermost,
                    const double3 &origin,
                    double out[16],
                    const char *what)
{
  const double *d = m;
  std::memcpy(out, d, 16 * sizeof(double));
  if (!affine(out)) {
    return bad(what, "the matrix isn't affine");
  }
  if (outermost) {
    for (int a = 0; a < 3; a++) {
      out[12 + a] -= origin[a];
    }
  }
  for (int i = 0; i < 16; i++) {
    if (!std::isfinite(out[i])) {
      return bad(what, "the matrix isn't finite");
    }
  }
  return Status();
}

} // namespace

Status DrawList::shape(const DrawStyle &style, ShapeKind kind, const mat4d &matrix)
{
  CDCC_TRY(checkStyle(style, "shape"));
  if (int32_t(kind) < int32_t(ShapeKind::Arrow) ||
      int32_t(kind) > int32_t(ShapeKind::Quad))
  {
    return bad("shape", "no such shape " + std::to_string(int32_t(kind)));
  }
  double m[16];
  CDCC_TRY(storedMatrix(matrix, depth_ == 0, origin_, m, "shape"));
  CDCC_TRY(room(kRecordHeaderBytes + 128, "shape"));
  std::memcpy(append(DrawKind::Shape, style, 0, uint32_t(kind), 128), m, 128);
  return Status();
}

Status DrawList::pushMatrix(const mat4d &matrix)
{
  double m[16];
  CDCC_TRY(storedMatrix(matrix, depth_ == 0, origin_, m, "pushMatrix"));
  CDCC_TRY(room(kRecordHeaderBytes + 128, "pushMatrix"));
  std::memcpy(append(DrawKind::PushMatrix, DrawStyle(), 0, 0, 128), m, 128);
  depth_++;
  return Status();
}

Status DrawList::popMatrix()
{
  if (depth_ == 0) {
    return bad("popMatrix", "no push to pop");
  }
  CDCC_TRY(room(kRecordHeaderBytes, "popMatrix"));
  append(DrawKind::PopMatrix, DrawStyle(), 0, 0, 0);
  depth_--;
  return Status();
}

Status
DrawList::circle2d(const DrawStyle &style, double x, double y, double radius, bool filled)
{
  CDCC_TRY(checkStyle(style, "circle2d"));
  if (!fitsFloat(x) || !fitsFloat(y) || !fitsFloat(radius) || radius < 0.0) {
    return bad("circle2d", "the centre or the radius isn't a finite float (radius >= 0)");
  }
  CDCC_TRY(room(kRecordHeaderBytes + 16, "circle2d"));
  uint8_t *p =
      append(DrawKind::Circle2D, style, filled ? drawflags::kFilled : uint8_t(0), 0, 12);
  put<float>(p, float(x));
  put<float>(p + 4, float(y));
  put<float>(p + 8, float(radius));
  return Status();
}

Status DrawList::line2d(const DrawStyle &style, std::span<const double> xy)
{
  CDCC_TRY(checkStyle(style, "line2d"));
  if (xy.size() % 2 != 0) {
    return bad("line2d", "an odd count of numbers");
  }
  for (double v : xy) {
    if (!fitsFloat(v)) {
      return bad("line2d", "a coordinate isn't a finite float");
    }
  }
  const uint64_t payload = uint64_t(xy.size()) * 4;
  CDCC_TRY(room(kRecordHeaderBytes + pad8(payload), "line2d"));
  uint8_t *p =
      append(DrawKind::Line2D, style, 0, uint32_t(xy.size() / 2), uint32_t(payload));
  for (double v : xy) {
    put<float>(p, float(v));
    p += 4;
  }
  return Status();
}

std::span<const uint8_t> DrawList::bytes() const
{
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(words_.data()),
                                  size_t(size_));
}

/* ---- for scripts ---- */

namespace {

Status pointsOf(const litestl::util::Vector<double> &xyz,
                litestl::util::Vector<double3> &out,
                const char *what)
{
  if (xyz.size() % 3 != 0) {
    return bad(what, "three numbers per point");
  }
  out.resize(xyz.size() / 3);
  for (int i = 0; i < int(out.size()); i++) {
    out[i] = double3(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]);
  }
  return Status();
}

} // namespace

Status DrawList::linesXyz(const DrawStyle &style,
                          const litestl::util::Vector<double> &xyz)
{
  litestl::util::Vector<double3> pts;
  CDCC_TRY(pointsOf(xyz, pts, "lines"));
  return lines(style, std::span<const double3>(pts.data(), pts.size()));
}

Status DrawList::pointsXyz(const DrawStyle &style,
                           const litestl::util::Vector<double> &xyz)
{
  litestl::util::Vector<double3> pts;
  CDCC_TRY(pointsOf(xyz, pts, "points"));
  return points(style, std::span<const double3>(pts.data(), pts.size()));
}

Status DrawList::trianglesXyz(const DrawStyle &style,
                              const litestl::util::Vector<double> &xyz,
                              const litestl::util::Vector<double> &colors)
{
  litestl::util::Vector<double3> pts;
  CDCC_TRY(pointsOf(xyz, pts, "triangles"));
  litestl::util::Vector<uint32_t> c;
  for (double v : colors) {
    if (!(v >= 0.0 && v <= double(UINT32_MAX) && v == std::floor(v))) {
      return bad("triangles", "a color isn't a whole number of 32 bits");
    }
    c.append(uint32_t(v));
  }
  return triangles(style,
                   std::span<const double3>(pts.data(), pts.size()),
                   std::span<const uint32_t>(c.data(), c.size()));
}

Status DrawList::shapeOf(const DrawStyle &style, int32_t kind, const mat4d &matrix)
{
  return shape(style, ShapeKind(kind), matrix);
}

Status DrawList::line2dXy(const DrawStyle &style, const litestl::util::Vector<double> &xy)
{
  return line2d(style, std::span<const double>(xy.data(), xy.size()));
}

int64_t DrawList::address() const
{
  return int64_t(reinterpret_cast<intptr_t>(words_.data()));
}

Status DrawList::read(int64_t offset, std::span<uint8_t> out) const
{
  if (offset < 0 || uint64_t(offset) > size_ || out.size() > size_ - uint64_t(offset)) {
    return Status::error(StatusCode::OutOfRange,
                         (std::to_string(out.size()) + " bytes at " +
                          std::to_string(offset) + " of " + std::to_string(size_))
                             .c_str());
  }
  std::memcpy(out.data(), bytes().data() + offset, out.size());
  return Status();
}

/* ---- DrawItem ---- */

float DrawItem::vertex(uint32_t i, int axis) const
{
  const uint32_t n = kind == DrawKind::Line2D ? 2 : 3;
  return get<float>(payload + 4 * (n * i + uint32_t(axis)));
}

uint32_t DrawItem::cornerColor(uint32_t i) const
{
  return get<uint32_t>(payload + 12 * aux + 4 * i);
}

mat4d DrawItem::matrix() const
{
  mat4d m;
  double *d = m;
  std::memcpy(d, payload, 128);
  return m;
}

} // namespace coredcc

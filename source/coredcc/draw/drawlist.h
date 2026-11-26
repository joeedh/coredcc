#pragma once

/** \file
 * DrawList: what tools and objects draw over the viewport, as plain bytes that a host
 * renders (architecture §10). The format is `docs/drawlist-format.md`, version 1.
 */

#include "coredcc/base/status.h"

#include "litestl/math/transform.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <span>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

using litestl::math::double3;
using litestl::math::mat4d;

/** The kinds of records. The numbers are the format's. */
enum class DrawKind : uint16_t {
  Lines = 1,
  Points = 2,
  Triangles = 3,
  Shape = 4,
  Circle2D = 5,
  Line2D = 6,
  PushMatrix = 7,
  PopMatrix = 8,
};

/** How a 3D record meets the depth buffer. XRay: visible parts as they are, hidden ones
 * faded. */
enum class DepthMode : int32_t { Test = 0, Always = 1, XRay = 2 };

/** Built-in shapes, defined in the format document (in their own space, about 1 unit). */
enum class ShapeKind : int32_t {
  Arrow = 1,
  Cone = 2,
  Cube = 3,
  Sphere = 4,
  Disc = 5,
  Ring = 6,
  Quad = 7,
};

/** What every record has besides its geometry. */
struct DrawStyle {
  /** RGBA8, straight alpha, red in the lowest byte. */
  uint32_t color = 0xffffffffu;
  /** Lines and outlines: logical pixels. Points: their diameter. */
  float width = 1.0f;
  /** A DepthMode. */
  int32_t depth = int32_t(DepthMode::Test);
  /** What the record draws (a gizmo's part, an element), for hosts and tests; 0 for none.
   */
  uint32_t id = 0;

  static litestl::binding::types::Struct<DrawStyle> *defineBindings();
};

/** Bits of a record's flags byte. */
namespace drawflags {
inline constexpr uint8_t kCornerColors = 1; /* Triangles: a color per corner */
inline constexpr uint8_t kFilled = 2;       /* Circle2D */
} // namespace drawflags

/**
 * An append-only list of records, with a double origin.
 *
 * - 3D coordinates are in the space of the innermost PushMatrix, or the world's. Outside
 *   any push a point is stored as the float of `p - origin`; inside one, as the float of
 *   `p`. The outermost push's matrix, and a Shape's outside any push, are stored with
 *   their translation minus the origin, in double. So nothing far from the origin is a
 *   float.
 * - 2D records are in device pixels of the View's space, y down.
 * - After `end` the list is frozen until the next `begin`: hosts read `bytes()` then, and
 *   take them again each frame.
 * - What can't be stored (a number that isn't finite, a count that doesn't fit, an append
 *   to a frozen list, a pop without a push) is InvalidArgument, and nothing is appended.
 */
class DrawList {
public:
  /** Format numbers. */
  static constexpr uint32_t kMagic = 0x4c444443u; /* "CDDL" */
  static constexpr uint16_t kVersion = 1;
  static constexpr int kHeaderBytes = 48;
  static constexpr int kRecordHeaderBytes = 24;
  /** No record may be larger, nor the whole list over 2^32 - 1 bytes. */
  static constexpr uint64_t kMaxRecordBytes = uint64_t(1) << 31;

  DrawList();

  /** Empties it, with this origin, and unfreezes it. InvalidArgument for an origin that
   * isn't finite, which is then (0, 0, 0). */
  Status begin(const double3 &origin);
  /** Freezes it. InvalidArgument if pushes are left; they are popped. */
  Status end();
  bool frozen() const
  {
    return frozen_;
  }
  const double3 &origin() const
  {
    return origin_;
  }
  /** Records so far. */
  int32_t recordCount() const
  {
    return records_;
  }
  /** How deep the matrix stack is now. */
  int32_t depth() const
  {
    return depth_;
  }

  /* ---- 3D ---- */

  /** Segments: two points each. */
  Status lines(const DrawStyle &style, std::span<const double3> pairs);
  Status points(const DrawStyle &style, std::span<const double3> points);
  /** Three corners each. `colors` has one per corner, or none. */
  Status triangles(const DrawStyle &style,
                   std::span<const double3> corners,
                   std::span<const uint32_t> colors = {});
  Status shape(const DrawStyle &style, ShapeKind kind, const mat4d &matrix);
  /** The current matrix becomes current · m. Matrices of pushes and shapes must be
   * affine (their last row 0, 0, 0, 1). */
  Status pushMatrix(const mat4d &m);
  Status popMatrix();

  /* ---- 2D ---- */

  /** `radius` in logical pixels. */
  Status circle2d(const DrawStyle &style, double x, double y, double radius, bool filled);
  /** A strip: x, y, x, y, ... */
  Status line2d(const DrawStyle &style, std::span<const double> xy);

  /** The list, header first. Valid until the next append or begin. */
  std::span<const uint8_t> bytes() const;

  /* ---- for scripts: flat numbers, since a script can't pass a list of vectors ---- */

  Status linesXyz(const DrawStyle &style, const litestl::util::Vector<double> &xyz);
  Status pointsXyz(const DrawStyle &style, const litestl::util::Vector<double> &xyz);
  Status trianglesXyz(const DrawStyle &style,
                      const litestl::util::Vector<double> &xyz,
                      const litestl::util::Vector<double> &colors);
  Status shapeOf(const DrawStyle &style, int32_t kind, const mat4d &matrix);
  Status line2dXy(const DrawStyle &style, const litestl::util::Vector<double> &xy);
  /** The bytes' address in the module's memory: for a view of WASM memory, taken again
   * each frame. Meaningless outside WASM. */
  int64_t address() const;
  int64_t size() const
  {
    return int64_t(size_);
  }
  /** Copies `out.size()` bytes from `offset`; OutOfRange past the end. */
  Status read(int64_t offset, std::span<uint8_t> out) const;
  static litestl::binding::types::Struct<DrawList> *defineBindings();

private:
  /* Checks that n more bytes fit, and that the list isn't frozen. */
  Status room(uint64_t recordBytes, const char *what) const;
  uint8_t *append(DrawKind kind,
                  const DrawStyle &style,
                  uint8_t flags,
                  uint32_t aux,
                  uint32_t payloadBytes);
  Status vertices(DrawKind kind,
                  const DrawStyle &style,
                  std::span<const double3> pts,
                  int multiple,
                  std::span<const uint32_t> colors,
                  const char *what);
  void finishHeader();

  /* 8-byte words, so that records are aligned. */
  litestl::util::Vector<uint64_t> words_;
  uint64_t size_ = 0;
  int32_t records_ = 0;
  double3 origin_;
  int32_t depth_ = 0;
  bool frozen_ = false;
};

/** One record, as DrawListReader found it. Pointers are into the bytes it read. */
struct DrawItem {
  DrawKind kind = DrawKind::Lines;
  DrawStyle style;
  uint8_t flags = 0;
  /** Lines, Points, Triangles, Line2D: the vertex count. Shape: its ShapeKind. */
  uint32_t aux = 0;
  /** Byte offset of the record in the list. */
  uint32_t offset = 0;
  /** The payload: floats (vertices, or a circle's x, y and radius), then for
   * kCornerColors one u32 per corner; doubles for a matrix. Unaligned reads only. */
  const uint8_t *payload = nullptr;
  uint32_t payloadBytes = 0;

  /** Vertex i's coordinate (0 to 2 in 3D; 0 and 1 in 2D). */
  float vertex(uint32_t i, int axis) const;
  uint32_t cornerColor(uint32_t i) const;
  /** A Shape's or a PushMatrix's matrix, column-major. */
  mat4d matrix() const;
};

/**
 * Reads and checks a list: the header, every record's size, alignment, kind, depth and
 * counts, and that pushes and pops balance. It never reads past the bytes it is given.
 */
class DrawListReader {
public:
  /** InvalidArgument, naming the offset, for anything that isn't a valid list. */
  Status read(std::span<const uint8_t> bytes);
  const double3 &origin() const
  {
    return origin_;
  }
  const litestl::util::Vector<DrawItem> &items() const
  {
    return items_;
  }

private:
  double3 origin_;
  litestl::util::Vector<DrawItem> items_;
};

} // namespace coredcc

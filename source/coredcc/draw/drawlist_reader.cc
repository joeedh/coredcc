/* DrawListReader: checks a list's bytes before anything reads them. */
#include "coredcc/draw/drawlist.h"

#include <cmath>
#include <cstring>
#include <string>

namespace coredcc {

namespace {

template <class T> T get(const uint8_t *p)
{
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

Status bad(uint64_t at, const std::string &why)
{
  return Status::error(StatusCode::InvalidArgument,
                       ("DrawList at byte " + std::to_string(at) + ": " + why).c_str());
}

uint64_t pad8(uint64_t n)
{
  return (n + 7) & ~uint64_t(7);
}

bool finiteFloats(const uint8_t *p, uint64_t count)
{
  for (uint64_t i = 0; i < count; i++) {
    if (!std::isfinite(get<float>(p + 4 * i))) {
      return false;
    }
  }
  return true;
}

bool finiteDoubles(const uint8_t *p, int count)
{
  for (int i = 0; i < count; i++) {
    if (!std::isfinite(get<double>(p + 8 * i))) {
      return false;
    }
  }
  return true;
}

/* A column-major matrix's last row is 0, 0, 0, 1. */
bool affineAt(const uint8_t *p)
{
  return get<double>(p + 8 * 3) == 0.0 && get<double>(p + 8 * 7) == 0.0 &&
         get<double>(p + 8 * 11) == 0.0 && get<double>(p + 8 * 15) == 1.0;
}

bool zeros(const uint8_t *p, uint64_t count)
{
  for (uint64_t i = 0; i < count; i++) {
    if (p[i] != 0) {
      return false;
    }
  }
  return true;
}

/* The payload's size that `item` needs, or an error. */
Status payloadOf(const DrawItem &item, uint64_t at, uint64_t &need)
{
  const uint8_t allowed = item.kind == DrawKind::Triangles  ? drawflags::kCornerColors
                          : item.kind == DrawKind::Circle2D ? drawflags::kFilled
                                                            : uint8_t(0);
  if ((item.flags & ~allowed) != 0) {
    return bad(at, "flags that this kind doesn't have");
  }
  const uint64_t n = item.aux;
  switch (item.kind) {
  case DrawKind::Lines:
  case DrawKind::Points:
  case DrawKind::Triangles: {
    const uint64_t multiple = item.kind == DrawKind::Lines       ? 2
                              : item.kind == DrawKind::Triangles ? 3
                                                                 : 1;
    if (n % multiple != 0) {
      return bad(at,
                 "a vertex count that isn't a multiple of " + std::to_string(multiple));
    }
    need = 12 * n + ((item.flags & drawflags::kCornerColors) ? 4 * n : 0);
    return Status();
  }
  case DrawKind::Shape:
    if (n < uint64_t(ShapeKind::Arrow) || n > uint64_t(ShapeKind::Quad)) {
      return bad(at, "no such shape " + std::to_string(n));
    }
    need = 128;
    return Status();
  case DrawKind::PushMatrix:
    need = 128;
    return Status();
  case DrawKind::PopMatrix:
    need = 0;
    return Status();
  case DrawKind::Circle2D:
    need = 12;
    return Status();
  case DrawKind::Line2D:
    need = 8 * n;
    return Status();
  }
  return bad(at, "no such kind " + std::to_string(int(item.kind)));
}

} // namespace

Status DrawListReader::read(std::span<const uint8_t> bytes)
{
  items_.clear();
  const uint8_t *b = bytes.data();
  const uint64_t size = bytes.size();
  if (size < uint64_t(DrawList::kHeaderBytes)) {
    return bad(0, "shorter than a header");
  }
  if (get<uint32_t>(b) != DrawList::kMagic) {
    return bad(0, "not a DrawList (magic)");
  }
  if (get<uint16_t>(b + 4) != DrawList::kVersion) {
    return bad(4, "version " + std::to_string(get<uint16_t>(b + 4)) + ", not 1");
  }
  if (get<uint16_t>(b + 6) != 0) {
    return bad(6, "header flags that don't exist");
  }
  if (get<uint64_t>(b + 40) != 0) {
    return bad(40, "a reserved word that isn't 0");
  }
  const uint32_t count = get<uint32_t>(b + 8);
  if (get<uint32_t>(b + 12) != size) {
    return bad(12,
               "a byte size of " + std::to_string(get<uint32_t>(b + 12)) + " in " +
                   std::to_string(size) + " bytes");
  }
  if (!finiteDoubles(b + 16, 3)) {
    return bad(16, "the origin isn't finite");
  }
  origin_ = double3(get<double>(b + 16), get<double>(b + 24), get<double>(b + 32));

  int64_t depth = 0;
  uint64_t at = DrawList::kHeaderBytes;
  while (at < size) {
    if (size - at < uint64_t(DrawList::kRecordHeaderBytes)) {
      return bad(at, "a record header past the end");
    }
    const uint8_t *r = b + at;
    DrawItem item;
    item.kind = DrawKind(get<uint16_t>(r));
    const uint8_t depthMode = get<uint8_t>(r + 2);
    item.flags = get<uint8_t>(r + 3);
    const uint64_t recordBytes = get<uint32_t>(r + 4);
    item.style.color = get<uint32_t>(r + 8);
    item.style.width = get<float>(r + 12);
    item.style.id = get<uint32_t>(r + 16);
    item.aux = get<uint32_t>(r + 20);
    item.style.depthMode = DepthMode(depthMode);
    item.offset = uint32_t(at);
    if (depthMode > uint8_t(DepthMode::XRay)) {
      return bad(at, "no such depth mode");
    }
    if (!std::isfinite(item.style.width) || item.style.width < 0.0f) {
      return bad(at, "a width that isn't a number of 0 or more");
    }
    if (recordBytes % 8 != 0 || recordBytes < uint64_t(DrawList::kRecordHeaderBytes) ||
        recordBytes > size - at)
    {
      return bad(at, "a record size of " + std::to_string(recordBytes));
    }
    uint64_t need = 0;
    CDCC_TRY(payloadOf(item, at, need));
    if (pad8(need) != recordBytes - DrawList::kRecordHeaderBytes) {
      return bad(at, "a record size that doesn't fit its count");
    }
    item.payload = r + DrawList::kRecordHeaderBytes;
    item.payloadBytes = uint32_t(need);
    if (!zeros(item.payload + need, pad8(need) - need)) {
      return bad(at, "padding that isn't zeros");
    }
    const bool isMatrix =
        item.kind == DrawKind::Shape || item.kind == DrawKind::PushMatrix;
    const uint64_t floats = isMatrix                           ? 0
                            : item.kind == DrawKind::Line2D    ? 2 * uint64_t(item.aux)
                            : item.kind == DrawKind::Circle2D  ? 3
                            : item.kind == DrawKind::PopMatrix ? 0
                                                               : 3 * uint64_t(item.aux);
    if (isMatrix ? !finiteDoubles(item.payload, 16) : !finiteFloats(item.payload, floats))
    {
      return bad(at, "a number that isn't finite");
    }
    if (isMatrix && !affineAt(item.payload)) {
      return bad(at, "a matrix that isn't affine");
    }
    if (item.kind == DrawKind::Circle2D && get<float>(item.payload + 8) < 0.0f) {
      return bad(at, "a negative radius");
    }
    if (item.kind == DrawKind::PushMatrix) {
      depth++;
    } else if (item.kind == DrawKind::PopMatrix && --depth < 0) {
      return bad(at, "a pop without a push");
    }
    items_.append(item);
    at += recordBytes;
  }
  if (items_.size() != count) {
    return bad(8,
               "a record count of " + std::to_string(count) + " for " +
                   std::to_string(items_.size()) + " records");
  }
  if (depth != 0) {
    return bad(size, std::to_string(depth) + " pushes without a pop");
  }
  return Status();
}

} // namespace coredcc

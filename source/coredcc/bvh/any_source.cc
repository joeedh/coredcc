#include "coredcc/bvh/any_source.h"

#include <string>

namespace coredcc {

namespace {
constexpr int kChunk = 4096;
}

Status AnyBvh::build(Runtime &rt, AnyBvhSource src, BvhOptions opts)
{
  if (!src) {
    return Status::error(StatusCode::InvalidArgument, "AnyBvh: a null source");
  }
  if (rt.hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "AnyBvh: a sticky error is pending; take it first");
  }
  const int n = src.size();
  if (rt.hasStickyError()) {
    return rt.takeStickyError();
  }
  if (n < 0) {
    return Status::error(StatusCode::InvalidArgument, "AnyBvh: a source of negative size");
  }
  BoxSource gathered;
  for (int first = 0; first < n; first += kChunk) {
    const int count = n - first < kChunk ? n - first : kChunk;
    const litestl::util::Vector<double> values = src.bounds(first, count);
    if (rt.hasStickyError()) {
      return rt.takeStickyError();
    }
    if (values.size() != size_t(count) * 6) {
      return Status::error(StatusCode::InvalidArgument,
                           ("AnyBvh: bounds(" + std::to_string(first) + ", " +
                            std::to_string(count) + ") gave " + std::to_string(values.size()) +
                            " values instead of " + std::to_string(count * 6))
                               .c_str());
    }
    for (int i = 0; i < count; i++) {
      Box3d box;
      for (int a = 0; a < 3; a++) {
        box.min[a] = values[i * 6 + a];
        box.max[a] = values[i * 6 + 3 + a];
      }
      gathered.append(box);
    }
  }
  return boxes_.build(gathered, opts);
}

} // namespace coredcc

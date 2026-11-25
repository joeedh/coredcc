#include "coredcc/lib/io/library_io.h"
#include "coredcc/lib/io/pack.h"

#include "litestl/io/compress.h"
#include "litestl/util/alloc.h"

#include <cstring>
#include <string>
#include <utility>

namespace coredcc {

using litestl::util::Vector;

void IoIssues::add(BlockId block, std::string_view blockName, Status why)
{
  total++;
  if (int(first.size()) < kMaxKept) {
    IoIssue issue;
    issue.block = block;
    issue.blockName = std::string(blockName).c_str();
    issue.why = std::move(why);
    first.append(std::move(issue));
  }
}

namespace detail::libio {

namespace {

Vector<uint8_t> copyOf(std::span<const uint8_t> bytes)
{
  Vector<uint8_t> out;
  out.resize<false>(bytes.size());
  if (!bytes.empty()) {
    memcpy(out.data(), bytes.data(), bytes.size());
  }
  return out;
}

} // namespace

Status probeMemory(uint64_t bytes)
{
  if (bytes < (uint64_t(1) << 20)) {
    return Status(); /* the budget bounds how many of these there are */
  }
  void *p = bytes < SIZE_MAX / 2 ? litestl::alloc::alloc("coredcc load probe", size_t(bytes)) :
                                   nullptr;
  if (!p) {
    return Status::error(StatusCode::OutOfRange, "not enough memory for the data");
  }
  litestl::alloc::release(p);
  return Status();
}

Stored pack(std::span<const uint8_t> bytes, bool fast)
{
  Stored s;
  s.raw = uint64_t(bytes.size());
  if (s.raw >= kCompressFrom && s.raw <= kMaxRaw) {
    Vector<uint8_t> packed;
    const size_t n = sculptcore::io::compressBlock(
        bytes.data(), bytes.size(), packed,
        fast ? sculptcore::io::kFastCompressLevel : sculptcore::io::kDefaultCompressLevel);
    if (n > 0 && n < s.raw && plausibleLz4(s.raw, n)) {
      s.lz4 = true;
      s.bytes = copyOf(std::span<const uint8_t>(packed.data(), n)); /* not its worst case */
      return s;
    }
  }
  s.bytes = copyOf(bytes);
  return s;
}

Status unpack(std::span<const uint8_t> stored,
              bool lz4,
              uint64_t raw,
              Budget &budget,
              Vector<uint8_t> &out)
{
  if (raw > kMaxRaw) {
    return Status::error(StatusCode::OutOfRange, "a block of data is too large");
  }
  const bool sizesFit = lz4 ? raw > 0 && !stored.empty() && stored.size() <= kMaxRaw &&
                                  plausibleLz4(raw, stored.size()) :
                              raw == stored.size();
  if (!sizesFit) {
    return Status::error(StatusCode::InvalidArgument, "a block of data says the wrong size");
  }
  const uint64_t cost = unpackCost(raw);
  if (!budget.take(cost)) {
    return Status::error(StatusCode::OutOfRange, "the data needs more memory than allowed");
  }
  CDCC_TRY(probeMemory(cost));
  if (!lz4) {
    out = copyOf(stored);
    return Status();
  }
  if (!sculptcore::io::decompressBlock(stored.data(), stored.size(), size_t(raw), out)) {
    out.clear_and_contract();
    return Status::error(StatusCode::InvalidArgument, "compressed data is damaged");
  }
  return Status();
}

} // namespace detail::libio

} // namespace coredcc

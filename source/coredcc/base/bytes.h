#pragma once

/** \file
 * Bounds-checked byte streams for files, clipboards and snapshots. Little-endian.
 *
 * The writer never emits what the reader rejects: text is any bytes without NUL (no UTF-8
 * check, no length limit but the bytes there are).
 */

#include "coredcc/base/status.h"

#include "litestl/util/vector.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace coredcc {

/**
 * Appends values to a buffer. The first failure sticks: later writes do nothing, and
 * status() reports it, so callers check once at the end.
 */
class BytesWriter {
 public:
  /** The default limit; going past it is OutOfRange. */
  static constexpr size_t kMaxBytes = sizeof(void *) == 4 ? size_t(256) << 20 : size_t(1) << 30;

  explicit BytesWriter(size_t maxBytes = kMaxBytes) : max_(maxBytes) {}

  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void i64(int64_t v);
  /** Keeps the bits (NaN payloads, -0.0). */
  void f64(double v);
  void boolean(bool v);
  /** A u32 length and the bytes. Stops at a NUL, as litestl strings do. */
  void str(std::string_view s);
  /** A u32 length and the bytes. */
  void blob(std::span<const uint8_t> bytes);
  /** The bytes alone. */
  void raw(std::span<const uint8_t> bytes);

  /** Starts a section the reader gets as one blob(); returns the token endSection() takes.
   * Sections nest. */
  size_t beginSection();
  /** Ends the section, filling in its length. */
  void endSection(size_t token);

  /** Keeps the first failure. An ok Status is ignored. */
  void fail(Status s);
  const Status &status() const
  {
    return status_;
  }
  const litestl::util::Vector<uint8_t> &data() const
  {
    return data_;
  }
  /** Takes the bytes and the failure state: the writer is empty and ok afterwards. */
  litestl::util::Vector<uint8_t> take();

 private:
  uint8_t *grow(size_t n);
  template <class T> void number(T v);

  litestl::util::Vector<uint8_t> data_;
  Status status_;
  size_t max_;
};

/**
 * Reads what BytesWriter wrote, from borrowed bytes that must outlive the reader and the
 * views it returns. Any input is safe: the first failure sticks with the offset it
 * happened at, and every later read gives zeros and empty views.
 */
class BytesReader {
 public:
  explicit BytesReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  int64_t i64();
  double f64();
  /** Anything but 0 or 1 is a failure. */
  bool boolean();
  /** A view into the bytes. A NUL inside is a failure. */
  std::string_view str();
  std::span<const uint8_t> blob();
  /** The next `n` bytes. */
  std::span<const uint8_t> raw(size_t n);
  /** A u32 item count for items of at least `minItemBytes` (more than 0) bytes each. Fails
   * when that many can't follow, so a loop over the count is bounded by the input. */
  uint32_t count(size_t minItemBytes);

  size_t remaining() const
  {
    return bytes_.size() - at_;
  }
  size_t offset() const
  {
    return at_;
  }
  bool atEnd() const
  {
    return remaining() == 0;
  }
  /** Keeps the first failure and its offset, and moves to the end. An ok Status is
   * ignored. */
  void fail(Status s);
  void fail(StatusCode code, const char *message);
  const Status &status() const
  {
    return status_;
  }
  bool ok() const
  {
    return status_.ok();
  }

 private:
  const uint8_t *take(size_t n);
  template <class T> T number();

  std::span<const uint8_t> bytes_;
  size_t at_ = 0;
  Status status_;
};

} // namespace coredcc
